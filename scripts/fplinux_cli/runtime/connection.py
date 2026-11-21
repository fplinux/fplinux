# SPDX-License-Identifier: GPL-2.0-only
"""Distinguish an interrupted SSH connection from normal exit or a new RAM boot."""

from __future__ import annotations

import re
from contextlib import suppress
from dataclasses import dataclass
from enum import StrEnum
from textwrap import dedent
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail, is_sha256, sha256_bytes

if TYPE_CHECKING:
    from collections.abc import Sequence

BOOT_IDENTITY_COMMAND = (
    "uname -r && tr '\\000' '\\n' </proc/device-tree/chosen/fplinux,boot-recipe"
)
_KERNEL_LOCALVERSION = re.compile(r"(-fplinux-[0-9a-f]{16})$")
_CARD_BUILD_COMMAND = (
    "set -eu\n"
    + BOOT_IDENTITY_COMMAND
    + "\n"
    + dedent(
        """
        tr '\\000' '\\n' </proc/device-tree/chosen/fplinux,boot-entry
        sed -n 's/^BUILD_ID=//p' /etc/os-release
        /usr/libexec/fplinux/card-boot inspect
        cat /run/fplinux-card/system/target

        device=$(awk '$5 == "/run/fplinux-card" { print $3 }' /proc/self/mountinfo)
        partition=$(readlink -f "/sys/dev/block/$device")
        card=${partition%/*}/device
        [ "$(cat "$card/type")" = SD ]
        cat "$card/cid"

        access=/etc/fplinux/card-access
        digest=$(sha256sum "$access/device-id")
        printf '%.32s\\n' "$digest"
        key=$(/usr/bin/dropbearkey -y -f "$access/host-key")
        printf '%s\\n' "$key" | awk '$1 == "ssh-ed25519" { print $2 }'
        """
    ).strip()
)


def device_identity(kernel_localversion: str, boot_recipe: str) -> str:
    """Bind the visible kernel identifier and the boot composition."""
    return sha256_bytes(f"{kernel_localversion}\n{boot_recipe}\n".encode("ascii"))


def _decode_device_identity(kernel_release: str, boot_recipe: str) -> str | None:
    if not is_sha256(boot_recipe):
        return None
    version = _KERNEL_LOCALVERSION.search(kernel_release)
    return None if version is None else device_identity(version[1], boot_recipe)


def observed_device_identity(lines: Sequence[str]) -> str | None:
    """Derive the identity from the authenticated phone's reported kernel and DTB."""
    if len(lines) != 2:
        return None
    kernel_release, boot_recipe = lines
    return _decode_device_identity(kernel_release, boot_recipe)


class CardBootEntry(StrEnum):
    """Boot entry used by an enrolled system card."""

    SD = "sd"
    USB = "usb"


class CardSlot(StrEnum):
    """A mounted slot in the system-card layout."""

    A = "a"
    B = "b"


@dataclass(frozen=True)
class RunningBuild:
    """Build facts read from the authenticated running system."""

    kernel_release: str
    build_id: str
    card_slot: CardSlot | None = None


@dataclass(frozen=True)
class RunningCard:
    """Identity of the mounted root and its enrollment, observed over pinned SSH."""

    build: RunningBuild
    device_identity: str
    entry: CardBootEntry
    target: str
    cid: str
    serial: str
    host_key: str


def _decode_card_slot(record: str) -> CardSlot | None:
    field, separator, value = record.partition("=")
    if field == "slot" and separator:
        with suppress(ValueError):
            return CardSlot(value)
    return None


def _decode_running_card(output: str) -> RunningCard | None:
    lines = output.splitlines()
    if len(lines) != 9:
        return None
    release, recipe, boot_entry, build_id, slot_record, target, cid, serial, host_key = lines
    identity = _decode_device_identity(release, recipe)
    slot = _decode_card_slot(slot_record)
    if identity is None or slot is None or re.fullmatch(r"[a-f0-9]{12}", build_id) is None:
        return None
    try:
        entry = CardBootEntry(boot_entry)
    except ValueError:
        return None
    return RunningCard(
        build=RunningBuild(kernel_release=release, build_id=build_id, card_slot=slot),
        device_identity=identity,
        entry=entry,
        target=target,
        cid=cid,
        serial=serial,
        host_key=host_key,
    )


def read_running_card(connection: tuple[Any, dict[str, Any]]) -> RunningCard:
    """Check the actual SD backing device and slot without confirming or changing it."""
    transport, session = connection
    result = transport.run_remote(
        session, _CARD_BUILD_COMMAND, capture_output=True, shared=True, timeout=10
    )
    if result.returncode:
        detail = result.stderr.strip()
        fail(detail or f"Cannot inspect the enrolled microSD root (exit {result.returncode})")
    card = _decode_running_card(result.stdout)
    if card is None:
        fail("Cannot verify the running microSD boot recipe and mounted slot")
    return card


def _decode_running_build(output: str, *, inspect_card: bool) -> RunningBuild:
    lines = output.splitlines()
    if not lines or not lines[0].strip():
        fail("Cannot read the running phone kernel release")
    kernel_release, *metadata = lines
    build_id = metadata[0].strip() if metadata else ""
    card_slot = None
    if inspect_card:
        if len(metadata) != 2 or (card_slot := _decode_card_slot(metadata[1])) is None:
            fail("Cannot verify the running microSD slot")
    return RunningBuild(
        kernel_release=kernel_release.strip(), build_id=build_id or "unknown", card_slot=card_slot
    )


def read_running_build(
    connection: tuple[Any, dict[str, Any]], *, inspect_card: bool = False
) -> RunningBuild:
    """Read existing system metadata with a bounded SSH request."""
    transport, session = connection
    command = "uname -r; sed -n 's/^BUILD_ID=//p' /etc/os-release"
    if inspect_card:
        command += " && /usr/libexec/fplinux/card-boot inspect"
    result = transport.run_remote(
        session,
        command,
        capture_output=True,
        shared=True,
        timeout=min(session.get("wait_seconds", 10), 10),
    )
    if result.returncode:
        detail = result.stderr.strip()
        fail(detail or f"Cannot read the running phone build (exit {result.returncode})")
    return _decode_running_build(result.stdout, inspect_card=inspect_card)


class ConnectionLostError(Exception):
    """Interactive SSH failed or its authenticated USB device disappeared."""


class SessionResumedError(Exception):
    """An existing Linux session returned while a new loader was still waiting."""

    def __init__(self, session: dict[str, Any]) -> None:
        """Retain the existing credentials for authentication after candidate cleanup."""
        super().__init__("The existing Linux session is available")
        self.session = session

# SPDX-License-Identifier: GPL-2.0-only
"""Distinguish an interrupted SSH connection from normal exit or a new RAM boot."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail, is_sha256, sha256_bytes

if TYPE_CHECKING:
    from collections.abc import Sequence

BOOT_IDENTITY_COMMAND = (
    "uname -r && tr '\\000' '\\n' </proc/device-tree/chosen/fplinux,boot-recipe"
)
_KERNEL_LOCALVERSION = re.compile(r"(-fplinux-[0-9a-f]{16})$")


def device_identity(kernel_localversion: str, boot_recipe: str) -> str:
    """Bind the visible kernel identifier and the boot composition."""
    return sha256_bytes(f"{kernel_localversion}\n{boot_recipe}\n".encode("ascii"))


def observed_device_identity(lines: Sequence[str]) -> str | None:
    """Derive the identity from the authenticated phone's reported kernel and DTB."""
    if len(lines) != 2 or not is_sha256(lines[1]):
        return None
    version = _KERNEL_LOCALVERSION.search(lines[0])
    return None if version is None else device_identity(version[1], lines[1])


@dataclass(frozen=True)
class RunningBuild:
    """Build facts read from the authenticated running system."""

    kernel_release: str
    build_id: str
    card_slot: str | None = None


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
    lines = result.stdout.splitlines()
    if not lines or not lines[0].strip():
        fail("Cannot read the running phone kernel release")
    build_id = lines[1].strip() if len(lines) > 1 and lines[1].strip() else "unknown"
    card_slot = None
    if inspect_card:
        if len(lines) != 3 or lines[2] not in {"slot=a", "slot=b"}:
            fail("Cannot verify the running microSD slot")
        card_slot = lines[2][-1]
    return RunningBuild(lines[0].strip(), build_id, card_slot)


class ConnectionLostError(Exception):
    """Interactive SSH failed or its authenticated USB device disappeared."""


class SessionResumedError(Exception):
    """An existing Linux session returned while a new loader was still waiting."""

    def __init__(self, session: dict[str, Any]) -> None:
        """Retain the existing credentials for authentication after candidate cleanup."""
        super().__init__("The existing Linux session is available")
        self.session = session

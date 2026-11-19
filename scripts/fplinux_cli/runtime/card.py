# SPDX-License-Identifier: GPL-2.0-only
"""Install release card assets using the authenticated NAND reader session."""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import tempfile
from contextlib import suppress
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from textwrap import dedent
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail, sha256_file

from .bundle import require_file
from .card_access import CARD_CID, prepare_card_access
from .card_media import (
    CARD_ASSETS,
    CARD_WRITER,
    CardMediaError,
    prepare_card_media,
    read_card_layout,
)
from .phone_data import preparation_progress
from .state import private_directory
from .workflow import run_phone

if TYPE_CHECKING:
    from collections.abc import Callable

    from fplinux_cli.device_data.inputs import FirmwareInput

    from .card_media import CardLayout
    from .events import RunLog

_CARD_DEVICE = re.compile(r"/dev/mmcblk[0-9]{1,2}")
_REMOTE_STAGING = re.compile(r"/tmp/fplinux-card\.[A-Za-z0-9]+")  # noqa: S108 -- remote mktemp.
_MIB = 1024 * 1024
_LIST_CARDS = dedent(
    """\
    if [ -e /run/fplinux-card-install.lock ]; then
        if ! (exec 9</run/fplinux-card-install.lock; busybox flock -n 9); then
            printf '%s\\n' 'A card writer is still active. Keep the phone powered.' >&2
            exit 1
        fi
    fi
    for slot in /sys/block/mmcblk*; do
        [ -f "$slot/device/type" ] && [ "$(cat "$slot/device/type")" = SD ] || continue
        device=/dev/${slot##*/}
        [ -b "$device" ] || continue
        printf '%s %s %s\\n' "$device" "$(cat "$slot/device/cid")" "$(cat "$slot/size")"
    done
    """
)


@dataclass(frozen=True)
class SdCard:
    """An SD device discovered inside one authenticated phone."""

    device: str
    cid: str
    bytes: int


def _read_cards(output: str, log: RunLog) -> list[SdCard]:
    cards = []
    for line in output.splitlines():
        fields = line.split()
        if (
            len(fields) != 3
            or _CARD_DEVICE.fullmatch(fields[0]) is None
            or CARD_CID.fullmatch(fields[1]) is None
            or not fields[2].isdigit()
        ):
            fail("The phone returned invalid SD card details")
        card = SdCard(fields[0], fields[1], int(fields[2]) * 512)
        cards.append(card)
        log.message(f"SD card {card.cid} ({card.bytes / _MIB:.0f} MiB) at {card.device}")
    if not cards:
        fail("No microSD card was found in the phone")
    return cards


def _select_card(
    cards: list[SdCard], erase_card: str | None, confirm: Callable[[SdCard], bool] | None
) -> SdCard:
    if erase_card is not None:
        matching = [card for card in cards if card.cid == erase_card]
        if len(matching) != 1:
            fail("--erase-card must match exactly one SD card in the phone")
        return matching[0]
    if len(cards) != 1 or confirm is None:
        fail("Select the card to erase with --erase-card CID")
    if not confirm(cards[0]):
        fail("Card installation cancelled")
    return cards[0]


def _manifest(bundle: Path) -> dict[str, Any]:
    path = bundle / "build-manifest.json"
    require_file(path)
    manifest = json.loads(path.read_bytes())
    if not isinstance(manifest, dict):
        fail("The build manifest must be an object")
    return manifest


def load_verified_card_kit(bundle: Path, card_bundle: Path) -> CardLayout:
    """Bind all card assets to the selected phone and RAM release before loading USB."""
    build = _manifest(bundle)
    files = _manifest(card_bundle).get("files")
    if not isinstance(files, dict):
        fail("The microSD kit has no build file inventory")
    for name in CARD_ASSETS:
        path = card_bundle / "card" / name
        require_file(path)
        record = files.get(f"card/{name}")
        if (
            not isinstance(record, dict)
            or path.stat().st_size != record.get("size")
            or sha256_file(path) != record.get("sha256")
        ):
            fail(f"The microSD asset differs from its build manifest: {name}")
    layout = read_card_layout(card_bundle / "card")
    if layout.target != build.get("target") or layout.release_id != build.get("release_id"):
        fail("The microSD kit does not match the selected phone and RAM release")
    return layout


def _write_card(  # noqa: PLR0913 -- admitted inputs and selected transport stay explicit.
    connection: tuple[Any, dict[str, Any]],
    card: SdCard,
    card_dir: Path,
    *,
    layout: CardLayout,
    firmware: dict[str, tuple[FirmwareInput, ...]],
    log: RunLog,
) -> None:
    transport, session = connection
    with tempfile.TemporaryDirectory(prefix="card-", dir=log.path.parent) as temp:
        local = private_directory(Path(temp))
        try:
            generated = prepare_card_media(card_dir, local, layout, firmware)
        except CardMediaError as error:
            fail(str(error))
        paths = (
            card_dir / "boot.xz",
            card_dir / "root.ext4.xz",
            card_dir / "board.tar",
            *generated,
            CARD_WRITER,
        )
        result = transport.run_remote(
            session, "mktemp -d /tmp/fplinux-card.XXXXXXXX", capture_output=True, shared=True
        )
        staging = result.stdout.strip()
        if result.returncode or _REMOTE_STAGING.fullmatch(staging) is None:
            fail("Cannot create a temporary directory on the phone")
        writer = ("sh", f"{staging}/{CARD_WRITER.name}")
        cleanup_names = [*(path.name for path in paths), "host-key"]
        writer_pending = False
        try:
            with prepare_card_access(connection, layout.target, card.cid, local) as access:
                cleanup_names.extend(path.name for path in access.files)
                log.message("Sending the system card files to the phone...")
                for path in (*paths, *access.files):
                    transport.upload(session, str(path), f"{staging}/{path.name}")
                result = transport.run_remote(
                    session,
                    shlex.join([*writer, "prepare-access", staging]),
                    capture_output=True,
                    shared=True,
                    timeout=30,
                )
                if result.returncode:
                    fail("Cannot generate the SSH host key in the phone's RAM reader")
                host_key = access.pin_host_key(result.stdout)
                command = shlex.join(
                    [
                        *writer,
                        card.device,
                        card.cid,
                        staging,
                        *layout.writer_arguments(),
                        sha256_file(card_dir / "board.tar"),
                        *(sha256_file(path) for path in generated),
                        host_key,
                    ]
                )
                log.message("Writing and reading back the SD card. Keep USB connected.")
                writer_pending = True
                access.write_started = True
                result = transport.run_remote(session, command, capture_output=True, shared=True)
                writer_pending = result.returncode == 255 or result.returncode < 0
                log.record(result.stderr.strip())
                if result.returncode:
                    detail = result.stderr.strip().splitlines()
                    fail("SD installation failed" + (f": {detail[-1]}" if detail else ""))
                shell = "PowerShell" if access.windows is not None else "a terminal"
                log.message(f"After booting the system card, connect from {shell} on this PC:")
                log.message(access.command)
        finally:
            if writer_pending:
                log.message(
                    "Card writing may still be in progress. Keep the phone powered and "
                    "check the card again after reconnecting. "
                    "The card SSH files are saved on this PC."
                )
            else:
                cleanup = shlex.join(
                    ["rm", "-f", "--", *(f"{staging}/{name}" for name in cleanup_names)]
                )
                with suppress(OSError, subprocess.TimeoutExpired, SystemExit):
                    transport.run_remote(
                        session,
                        f"{cleanup} && rmdir -- {staging}",
                        capture_output=True,
                        shared=True,
                        timeout=5,
                    )


def _use_reader(  # noqa: PLR0913 -- inspector and writer share the selected session.
    connection: tuple[Any, dict[str, Any]],
    firmware: dict[str, tuple[FirmwareInput, ...]],
    log: RunLog,
    *,
    card_dir: Path,
    layout: CardLayout,
    inspect_only: bool,
    erase_card: str | None,
    confirm: Callable[[SdCard], bool] | None,
) -> bool:
    transport, session = connection
    listing = transport.run_remote(session, _LIST_CARDS, capture_output=True, shared=True)
    if listing.returncode:
        detail = listing.stderr.strip().splitlines()
        fail("Cannot inspect the SD card" + (f": {detail[-1]}" if detail else ""))
    cards = _read_cards(listing.stdout, log)
    if inspect_only:
        return False
    card = _select_card(cards, erase_card, confirm)
    if card.bytes < layout.capacity_bytes:
        fail("The selected SD card is too small")
    with preparation_progress(connection, log, initial="card-sending"):
        _write_card(connection, card, card_dir, layout=layout, firmware=firmware, log=log)
    return True


def run_card(  # noqa: PLR0913 -- input selection, session and consent are independent.
    bundle: Path,
    data: Path,
    logs: Path,
    *,
    card_bundle: Path | None = None,
    inspect_only: bool = False,
    erase_card: str | None = None,
    confirm: Callable[[SdCard], bool] | None = None,
    phone_data: Path | None = None,
    from_dump: Path | None = None,
    events: Path | None = None,
    loader_trace: bool = False,
) -> int:
    """Start or reuse one RAM reader for card discovery, preparation and writing."""
    if erase_card is not None and CARD_CID.fullmatch(erase_card) is None:
        fail("--erase-card must be a 32-digit lowercase SD card CID")
    if not inspect_only and erase_card is None and confirm is None:
        fail("card install requires --erase-card CID")
    assets = bundle if card_bundle is None else card_bundle
    try:
        layout = load_verified_card_kit(bundle, assets)
    except (OSError, ValueError, CardMediaError) as error:
        fail(str(error))
    return run_phone(
        bundle,
        data,
        logs,
        phone_data=phone_data,
        from_dump=from_dump,
        events=events,
        loader_trace=loader_trace,
        install_card=partial(
            _use_reader,
            card_dir=assets / "card",
            layout=layout,
            inspect_only=inspect_only,
            erase_card=erase_card,
            confirm=confirm,
        ),
    )

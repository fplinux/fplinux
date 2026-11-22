# SPDX-License-Identifier: GPL-2.0-only
"""Inspect and install SD cards through the authenticated RAM reader session."""

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

from fplinux_cli.artifacts.card_kit import (
    CARD_APK_INDEX,
    CARD_APK_KEY,
    CARD_APK_PREFLIGHT,
    CARD_DEFAULT_APPS,
    CARD_MEMBERS,
    CardKitError,
    card_package_members,
    read_card_layout,
)
from fplinux_cli.common import fail, sha256_file

from .bundle import require_file
from .card_access import CARD_CID, prepare_card_access
from .card_media import prepare_card_media, prepare_card_update_media, write_card_archive
from .operations import card_selection_error
from .phone_data import preparation_progress
from .state import private_directory
from .workflow import run_phone

if TYPE_CHECKING:
    from collections.abc import Callable

    from fplinux_cli.artifacts.card_kit import CardLayout
    from fplinux_cli.device_data.inputs import FirmwareInput

    from .events import RunLog
    from .operations import CardAction

_CARD_DEVICE = re.compile(r"/dev/mmcblk[0-9]{1,2}")
_CARD_WRITER = Path(__file__).with_name("install-card.sh")
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
    cards: list[SdCard],
    cid: str | None,
    confirm: Callable[[SdCard], bool] | None,
    *,
    action: CardAction,
) -> SdCard:
    option = "--erase-card" if action == "install" else "--card"
    if cid is not None:
        matching = [card for card in cards if card.cid == cid]
        if len(matching) != 1:
            fail(f"{option} must match exactly one SD card in the phone")
        return matching[0]
    if len(cards) != 1 or confirm is None:
        fail(f"Select the card with {option} CID")
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
    for relative in CARD_MEMBERS + card_package_members(files):
        path = card_bundle / relative
        require_file(path)
        record = files.get(relative)
        if (
            not isinstance(record, dict)
            or path.stat().st_size != record.get("size")
            or sha256_file(path) != record.get("sha256")
        ):
            name = relative.removeprefix("card/")
            fail(f"The microSD asset differs from its build manifest: {name}")
    layout = read_card_layout(card_bundle / "card")
    if layout.target != build.get("target") or layout.release_id != build.get("release_id"):
        fail("The microSD kit does not match the selected phone and RAM release")
    return layout


def _writer_arguments(layout: CardLayout) -> tuple[str, ...]:
    return (
        str(layout.boot_offset // _MIB),
        str(layout.boot_size // _MIB),
        str(layout.root_offset // _MIB),
        str(layout.root_size // _MIB),
        str(layout.boot_prefix_size // _MIB),
        layout.boot_sha256,
        layout.boot_file_sha256,
        layout.root_sha256,
        layout.target,
        layout.release_id,
    )


def _write_card(  # noqa: PLR0913 -- admitted inputs and selected transport stay explicit.
    connection: tuple[Any, dict[str, Any]],
    card: SdCard,
    bundle: Path,
    *,
    layout: CardLayout,
    firmware: dict[str, tuple[FirmwareInput, ...]],
    log: RunLog,
) -> None:
    transport, session = connection
    card_dir = bundle / "card"
    defaults = card_dir / CARD_DEFAULT_APPS
    members = (f"card/{CARD_APK_INDEX}", f"card/{CARD_APK_KEY}")
    repository = {
        Path(name).name: bundle / name
        for name in (*members, *card_package_members(_manifest(bundle)["files"]))
    }
    with tempfile.TemporaryDirectory(prefix="card-", dir=log.path.parent) as temp:
        local = private_directory(Path(temp))
        try:
            generated = prepare_card_media(card_dir, local, layout, firmware)
        except CardKitError as error:
            fail(str(error))
        apk_checksums = local / "apks.sha256"
        apk_checksums.write_text(
            "".join(f"{sha256_file(path)}  {name}\n" for name, path in sorted(repository.items())),
            encoding="ascii",
            newline="\n",
        )
        paths = (
            card_dir / "boot.xz",
            card_dir / "root.ext4.xz",
            card_dir / "board.tar",
            *generated,
            defaults,
            apk_checksums,
            _CARD_WRITER,
            _CARD_WRITER.with_name("card-slots.sh"),
            _CARD_WRITER.with_name("card-apk-world.sh"),
        )
        result = transport.run_remote(
            session, "mktemp -d /tmp/fplinux-card.XXXXXXXX", capture_output=True, shared=True
        )
        staging = result.stdout.strip()
        if result.returncode or _REMOTE_STAGING.fullmatch(staging) is None:
            fail("Cannot create a temporary directory on the phone")
        writer = ("sh", f"{staging}/{_CARD_WRITER.name}")
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
                        *_writer_arguments(layout),
                        sha256_file(card_dir / "board.tar"),
                        *(sha256_file(path) for path in generated),
                        sha256_file(defaults),
                        sha256_file(apk_checksums),
                        host_key,
                    ]
                )
                log.message("Writing and reading back the SD card. Keep USB connected.")
                writer_pending = True
                access.write_started = True
                result = transport.send_remote(
                    session, command, partial(write_card_archive, repository)
                )
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


def _list_cards(connection: tuple[Any, dict[str, Any]], log: RunLog) -> list[SdCard]:
    transport, session = connection
    listing = transport.run_remote(session, _LIST_CARDS, capture_output=True, shared=True)
    if listing.returncode:
        detail = listing.stderr.strip().splitlines()
        fail("Cannot inspect the SD card" + (f": {detail[-1]}" if detail else ""))
    return _read_cards(listing.stdout, log)


def _inspect_card(connection: tuple[Any, dict[str, Any]], log: RunLog) -> None:
    _list_cards(connection, log)


def _install_card(  # noqa: PLR0913 -- the kit and consent inputs stay explicit.
    connection: tuple[Any, dict[str, Any]],
    firmware: dict[str, tuple[FirmwareInput, ...]],
    log: RunLog,
    *,
    bundle: Path,
    layout: CardLayout,
    erase_card: str | None,
    confirm: Callable[[SdCard], bool] | None,
) -> None:
    cards = _list_cards(connection, log)
    card = _select_card(cards, erase_card, confirm, action="install")
    if card.bytes < layout.capacity_bytes:
        fail("The selected SD card is too small")
    log.message(f"Shared files (FAT32 partition): {layout.boot_size // _MIB} MiB")
    log.message(
        f"FPLinux (ext4 partition): {layout.root_size // _MIB} MiB initially, "
        f"{(card.bytes - layout.root_offset) // _MIB} MiB after first-boot expansion"
    )
    if erase_card is None and (confirm is None or not confirm(card)):
        fail("Card installation cancelled")
    with preparation_progress(connection, log, initial="card-sending"):
        _write_card(connection, card, bundle, layout=layout, firmware=firmware, log=log)


def _change_card(  # noqa: PLR0913 -- selected assets, consent and transport stay independent.
    connection: tuple[Any, dict[str, Any]],
    firmware: dict[str, tuple[FirmwareInput, ...]],
    log: RunLog,
    *,
    bundle: Path,
    layout: CardLayout,
    action: CardAction,
    cid: str | None,
    confirm: Callable[[SdCard], bool] | None,
    build_id: str | None,
) -> None:
    from .card_update import rollback_card, update_card  # noqa: PLC0415 -- write actions only.

    card = _select_card(_list_cards(connection, log), cid, confirm, action=action)
    if cid is None and (confirm is None or not confirm(card)):
        fail(f"Card {action} cancelled")
    if card.bytes < layout.capacity_bytes:
        fail("The selected SD card is too small")
    if build_id is not None:
        rollback_card(connection, card, layout, build_id=build_id, log=log)
        return

    card_dir = bundle / "card"
    apk_members = card_package_members(_manifest(bundle)["files"])
    with tempfile.TemporaryDirectory(prefix="card-media-", dir=log.path.parent) as temporary:
        destination = private_directory(Path(temporary))
        try:
            checksums, fit = prepare_card_update_media(card_dir, destination, layout, firmware)
        except CardKitError as error:
            fail(str(error))
        update_card(
            connection,
            card,
            layout,
            assets={
                "root.ext4.xz": card_dir / "root.ext4.xz",
                "board.tar": card_dir / "board.tar",
                checksums.name: checksums,
                fit.name: fit,
                **{name: card_dir / name for name in CARD_APK_PREFLIGHT},
                **{name.removeprefix("card/"): bundle / name for name in apk_members},
            },
            log=log,
        )


def run_card(  # noqa: PLR0913 -- input selection, session and consent are independent.
    bundle: Path,
    data: Path,
    logs: Path,
    *,
    card_bundle: Path | None = None,
    action: CardAction = "install",
    erase_card: str | None = None,
    card_cid: str | None = None,
    build_id: str | None = None,
    confirm: Callable[[SdCard], bool] | None = None,
    phone_data: Path | None = None,
    from_dump: Path | None = None,
    events: Path | None = None,
    loader_trace: bool = False,
) -> int:
    """Start or reuse one RAM reader for card discovery, preparation and writing."""
    error = card_selection_error(
        action, erase_card, card_cid, build_id, may_confirm=confirm is not None
    )
    if error is not None:
        fail(error)
    if action == "inspect":
        return run_phone(
            bundle,
            data,
            logs,
            phone_data=phone_data,
            from_dump=from_dump,
            events=events,
            loader_trace=loader_trace,
            inspect_card=_inspect_card,
        )
    assets = bundle if card_bundle is None else card_bundle
    try:
        layout = load_verified_card_kit(bundle, assets)
    except (OSError, ValueError, CardKitError) as error:
        fail(str(error))
    if action == "install":
        write = partial(
            _install_card,
            bundle=assets,
            layout=layout,
            erase_card=erase_card,
            confirm=confirm,
        )
    else:
        write = partial(
            _change_card,
            bundle=assets,
            layout=layout,
            action=action,
            cid=card_cid,
            confirm=confirm,
            build_id=build_id,
        )
    return run_phone(
        bundle,
        data,
        logs,
        phone_data=phone_data,
        from_dump=from_dump,
        events=events,
        loader_trace=loader_trace,
        install_card=write,
    )

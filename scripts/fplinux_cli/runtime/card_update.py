# SPDX-License-Identifier: GPL-2.0-only
"""Run an update or rollback inside one authenticated RAM reader operation."""

from __future__ import annotations

import re
import shlex
import stat
import subprocess
import tarfile
import tempfile
from contextlib import suppress
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.card_kit import CARD_APK_INDEX, CARD_APK_KEY, CARD_APK_STATE
from fplinux_cli.common import fail, sha256_file

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping
    from typing import IO

    from fplinux_cli.artifacts.card_kit import CardLayout

    from .card import SdCard
    from .events import RunLog

CARD_UPDATE_READER = Path(__file__).with_name("card-update-reader.sh")
CARD_UPDATE_STAGE = Path(__file__).with_name("update-card.sh")
CARD_APK_WORLD = Path(__file__).with_name("card-apk-world.sh")
CARD_SLOTS = Path(__file__).with_name("card-slots.sh")
_REMOTE_WORK = re.compile(r"/tmp/fplinux-card-update\.[A-Za-z0-9]+")  # noqa: S108 -- remote mktemp.
_BUILD_ID = re.compile(r"[a-f0-9]{12}")
_PREFLIGHT_ASSETS = (CARD_APK_STATE, CARD_APK_INDEX, CARD_APK_KEY)
_UPDATE_ASSETS = {
    "root.ext4.xz",
    "board.tar",
    "board.sha256",
    "fit.itb",
    *_PREFLIGHT_ASSETS,
}
_APK_ASSET = re.compile(r"apks/[a-zA-Z0-9][a-zA-Z0-9._+-]{0,95}\.apk")
_UNCERTAIN = (
    "The reader may still be working on the SD card. Keep the phone powered and "
    "check the card after reconnecting."
)


def _write_archive(assets: Mapping[str, Path], destination: IO[bytes]) -> None:
    with tarfile.open(fileobj=destination, mode="w|", format=tarfile.USTAR_FORMAT) as archive:
        for name, path in sorted(assets.items()):
            with path.open("rb") as source:
                member = tarfile.TarInfo(name)
                member.size = path.stat().st_size
                member.mode = 0o600
                archive.addfile(member, source)


def _reader_arguments(action: str, card: SdCard, layout: CardLayout, build_id: str) -> list[str]:
    if _BUILD_ID.fullmatch(build_id) is None:
        fail("The system card action needs a 12-digit lowercase Build ID")
    return [
        action,
        card.device,
        card.cid,
        layout.target,
        build_id,
        str(layout.disk_signature),
        str(layout.boot_offset // 512),
        str(layout.boot_size // 512),
        str(layout.root_offset // 512),
        str(layout.root_size // 512),
    ]


def _run_reader(
    connection: tuple[Any, dict[str, Any]],
    arguments: list[str],
    log: RunLog,
    *,
    files: tuple[Path, ...],
    write: Callable[[IO[bytes]], None] | None = None,
) -> str:
    transport, session = connection
    result: subprocess.CompletedProcess[str] = transport.run_remote(
        session,
        "mktemp -d /tmp/fplinux-card-update.XXXXXXXX",
        capture_output=True,
        shared=True,
        timeout=15,
    )
    work = result.stdout.strip()
    if result.returncode or _REMOTE_WORK.fullmatch(work) is None:
        fail("Cannot create temporary reader storage in the phone")
    tools = (CARD_UPDATE_READER, CARD_SLOTS, *files)
    started = False
    try:
        for path in tools:
            transport.upload(session, str(path), f"{work}/{path.name}")
        command = shlex.join(["sh", f"{work}/{CARD_UPDATE_READER.name}", *arguments])
        log.message("Using the system card in the RAM reader. Keep USB connected.")
        started = True
        if write is None:
            result = transport.run_remote(session, command, capture_output=True, shared=True)
        else:
            result = transport.send_remote(session, command, write)
    except KeyboardInterrupt, OSError, subprocess.TimeoutExpired, SystemExit:
        if started:
            log.message(_UNCERTAIN)
        raise
    finally:
        if not started:
            cleanup = shlex.join(["rm", "-f", "--", *(f"{work}/{path.name}" for path in tools)])
            with suppress(OSError, subprocess.TimeoutExpired, SystemExit):
                transport.run_remote(
                    session,
                    f"{cleanup} && rmdir -- {work}",
                    capture_output=True,
                    shared=True,
                    timeout=5,
                )
    details = result.stderr.strip()
    if details:
        log.record(details)
    if result.returncode:
        if result.returncode >= 128 or result.returncode < 0:
            log.message(_UNCERTAIN)
        detail = details.splitlines()
        fail("System card operation failed" + (f": {detail[-1]}" if detail else ""))
    return result.stdout.strip()


def update_card(
    connection: tuple[Any, dict[str, Any]],
    card: SdCard,
    layout: CardLayout,
    *,
    assets: Mapping[str, Path],
    log: RunLog,
) -> None:
    """Transfer a verified release payload directly to ext4, then publish its staged pair."""
    if not _UPDATE_ASSETS.issubset(assets) or any(
        name not in _UPDATE_ASSETS and _APK_ASSET.fullmatch(name) is None for name in assets
    ):
        fail("The system card update has an invalid file inventory")
    arguments = _reader_arguments("update", card, layout, layout.release_id)
    digests = {}
    sizes = {}
    for name, path in assets.items():
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode):
            fail(f"The system card update asset is not a regular file: {name}")
        digests[name] = sha256_file(path)
        sizes[name] = info.st_size
    # The stock filesystem image and candidate tree coexist. APK state stays in RAM.
    payload_size = sum(sizes.values()) - sizes[CARD_APK_STATE]
    required = payload_size + 2 * layout.root_size + sizes["board.tar"]
    arguments.extend(
        (
            str(required),
            digests["fit.itb"],
            layout.root_sha256,
            digests["board.tar"],
            digests["board.sha256"],
        )
    )
    with tempfile.TemporaryDirectory(prefix="card-update-", dir=log.path.parent) as directory:
        work = Path(directory)
        manifest = work / "transfer.sha256"
        manifest.write_text(
            "".join(
                f"{digests[name]}  {name}\n" for name in sorted(digests) if name != CARD_APK_STATE
            ),
            encoding="ascii",
            newline="\n",
        )
        preflight = work / "apk-preflight.tar"
        with preflight.open("wb") as stream:
            _write_archive({Path(name).name: assets[name] for name in _PREFLIGHT_ASSETS}, stream)
        payload = {name: path for name, path in assets.items() if name not in _PREFLIGHT_ASSETS}
        result = _run_reader(
            connection,
            arguments,
            log,
            files=(CARD_UPDATE_STAGE, CARD_APK_WORLD, manifest, preflight),
            write=partial(_write_archive, payload),
        )
    if result == f"updated {layout.release_id}":
        log.message(f"System card build {layout.release_id} was written and the card unmounted.")
    elif result == f"active {layout.release_id}":
        log.message(f"System card build {layout.release_id} is already selected.")
    else:
        log.message(_UNCERTAIN)
        fail("The reader returned an invalid system card update result")


def rollback_card(
    connection: tuple[Any, dict[str, Any]],
    card: SdCard,
    layout: CardLayout,
    *,
    build_id: str,
    log: RunLog,
) -> None:
    """Select the requested confirmed pair even if the currently selected FIT is missing."""
    arguments = _reader_arguments("rollback", card, layout, build_id)
    if _run_reader(connection, arguments, log, files=()) != f"rolled-back {build_id}":
        log.message(_UNCERTAIN)
        fail("The reader returned an invalid system card rollback result")
    log.message(f"System card build {build_id} was selected and the card unmounted.")

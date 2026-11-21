# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- card media failures use exact diagnostics.
"""Prepare a release card kit using the admitted data from one phone."""

from __future__ import annotations

import hashlib
import tarfile
from io import BytesIO
from typing import TYPE_CHECKING

from fplinux_cli.artifacts.card_kit import FIT_AUDIO_CPIO_BYTES, CardKitError, verify_fit_slot
from fplinux_cli.common import sha256_bytes
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.device_data.inputs import firmware_storage, rootfs_firmware_inputs

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence
    from pathlib import Path
    from typing import IO, TextIO

    from fplinux_cli.artifacts.card_kit import CardLayout
    from fplinux_cli.device_data.inputs import FirmwareInput


def write_card_archive(assets: Mapping[str, Path], destination: IO[bytes]) -> None:
    """Stream regular files under the supplied archive names."""
    with tarfile.open(fileobj=destination, mode="w|", format=tarfile.USTAR_FORMAT) as archive:
        for name, path in sorted(assets.items()):
            with path.open("rb") as source:
                member = tarfile.TarInfo(name)
                member.size = path.stat().st_size
                member.mode = 0o600
                archive.addfile(member, source)


def _personalize_fit(
    template: bytes, layout: CardLayout, firmware: Mapping[str, Sequence[FirmwareInput]]
) -> bytes:
    data_offset, hash_offset = layout.fit_ramdisk_offset, layout.fit_hash_offset
    data_end = data_offset + layout.fit_ramdisk_size
    audio_offset = data_end - FIT_AUDIO_CPIO_BYTES
    ramdisk = template[data_offset:data_end]
    verify_fit_slot(template, ramdisk, data_offset, hash_offset)
    empty = create_device_data_initramfs({}).ljust(FIT_AUDIO_CPIO_BYTES, b"\0")
    if template[audio_offset:data_end] != empty:
        raise CardKitError("Release FIT has no empty early audio archive")
    audio = create_device_data_initramfs({"audio-profile": firmware.get("audio-profile", ())})
    if len(audio) > FIT_AUDIO_CPIO_BYTES:
        raise CardKitError("Prepared audio firmware exceeds the release FIT ramdisk slot")
    fit = bytearray(template)
    fit[audio_offset:data_end] = audio.ljust(FIT_AUDIO_CPIO_BYTES, b"\0")
    digest = hashlib.sha256(memoryview(fit)[data_offset:data_end]).digest()
    fit[hash_offset : hash_offset + len(digest)] = digest
    return bytes(fit)


def _write_board_checksums(archive: Path, records: TextIO, *, prefix: str) -> None:
    try:
        with tarfile.open(archive, "r:") as board:
            for member in board:
                if not member.isfile():
                    continue
                contents = board.extractfile(member)
                if contents is None:
                    raise CardKitError(f"Cannot read board archive file {member.name}")
                with contents:
                    digest = sha256_bytes(contents.read())
                records.write(f"{digest}  {prefix}{member.name}\n")
    except tarfile.TarError as error:
        raise CardKitError(f"Cannot read the board archive: {error}") from error


def _write_personal_archive(
    archive: Path, records: TextIO, firmware: Mapping[str, Sequence[FirmwareInput]]
) -> None:
    entries: dict[str, bytes] = {}
    for item in rootfs_firmware_inputs(firmware):
        name, contents = firmware_storage(item)
        path = f"lib/firmware/{name}"
        if path in entries:
            raise CardKitError(f"Firmware destination already exists: {path}")
        entries[path] = contents
    with tarfile.open(archive, "x", format=tarfile.USTAR_FORMAT) as output:
        for path, contents in sorted(entries.items()):
            member = tarfile.TarInfo(path)
            member.mode = 0o600
            member.size = len(contents)
            output.addfile(member, BytesIO(contents))
            records.write(f"{sha256_bytes(contents)}  data/{path}\n")


def prepare_card_media(
    card_dir: Path,
    destination: Path,
    layout: CardLayout,
    firmware: Mapping[str, Sequence[FirmwareInput]],
) -> tuple[Path, Path, Path]:
    """Create the personal tar, ext4 file checksums and FIT for phone staging."""
    archive = destination / "personal.tar"
    checksums = destination / "installed.sha256"
    fit = destination / "fit.itb"
    fit.write_bytes(_personalize_fit((card_dir / "fit.itb").read_bytes(), layout, firmware))
    with checksums.open("x", encoding="utf-8", newline="\n") as records:
        _write_board_checksums(card_dir / "board.tar", records, prefix="system/a/root/")
        _write_personal_archive(archive, records, firmware)
    return archive, checksums, fit


def prepare_card_update_media(
    card_dir: Path,
    destination: Path,
    layout: CardLayout,
    firmware: Mapping[str, Sequence[FirmwareInput]],
) -> tuple[Path, Path]:
    """Personalize the new FIT and check the board layer relative to either slot."""
    checksums = destination / "board.sha256"
    fit = destination / "fit.itb"
    fit.write_bytes(_personalize_fit((card_dir / "fit.itb").read_bytes(), layout, firmware))
    with checksums.open("x", encoding="utf-8", newline="\n") as records:
        _write_board_checksums(card_dir / "board.tar", records, prefix="")
    return checksums, fit

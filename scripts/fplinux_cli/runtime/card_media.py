# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- card media failures use exact diagnostics.
"""Prepare a release card kit using the admitted data from one phone."""

from __future__ import annotations

import hashlib
import struct
import tarfile
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import is_sha256, read_json_object, sha256_bytes
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.device_data.inputs import firmware_storage, rootfs_firmware_inputs

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence
    from typing import TextIO

    from fplinux_cli.device_data.inputs import FirmwareInput

CARD_WRITER = Path(__file__).with_name("install-card.sh")
CARD_ASSETS = ("boot.xz", "root.ext4.xz", "board.tar", "fit.itb", "layout.json")
CARD_FIT_RAMDISK_SIZE = 4096
_MIB = 1024 * 1024
_FDT_MAGIC = 0xD00DFEED
_FDT_HEADER_SIZE = 40
_FDT_PROPERTY_TOKEN = 3
_FDT_PROPERTY_HEADER_SIZE = 12
_FIT_FILENAME = "FPLINUX.ITB"


class CardMediaError(RuntimeError):
    """A release card kit or its personalized contents are invalid."""


@dataclass(frozen=True)
class CardLayout:
    """The target's fixed FAT and ext4 ranges, raw digests and inline FIT slot."""

    target: str
    release_id: str
    boot_offset: int
    boot_size: int
    root_offset: int
    root_size: int
    boot_sha256: str
    root_sha256: str
    fit_filename: str
    fit_ramdisk_offset: int
    fit_ramdisk_size: int
    fit_hash_offset: int

    @property
    def capacity_bytes(self) -> int:
        """Return the minimum card capacity for the complete layout."""
        return self.root_offset + self.root_size

    def writer_arguments(self) -> tuple[str, ...]:
        """Return the fixed media arguments for the phone-side writer."""
        return (
            str(self.boot_offset // _MIB),
            str(self.boot_size // _MIB),
            str(self.root_offset // _MIB),
            str(self.root_size // _MIB),
            self.boot_sha256,
            self.root_sha256,
            self.fit_filename,
        )


def _layout_integer(record: dict[str, object], field: str, *, alignment: int) -> int:
    value = record.get(field)
    if type(value) is not int or value <= 0 or value % alignment:
        raise CardMediaError(f"Card layout has an invalid {field}")
    return value


def _layout_digest(record: dict[str, object], field: str) -> str:
    value = record.get(field)
    if not isinstance(value, str) or not is_sha256(value):
        raise CardMediaError(f"Card layout has an invalid {field}")
    return value


def read_card_layout(card_dir: Path) -> CardLayout:
    """Read the byte ranges and identities from the admitted kit's layout."""
    record = read_json_object(card_dir / "layout.json")
    if record is None:
        raise CardMediaError("Cannot read the release card layout")
    target = record.get("target")
    release_id = record.get("release_id")
    if (
        not isinstance(target, str)
        or not target
        or not isinstance(release_id, str)
        or not release_id
    ):
        raise CardMediaError("Card layout lacks a phone model or release identity")
    layout = CardLayout(
        target=target,
        release_id=release_id,
        boot_offset=_layout_integer(record, "boot_offset", alignment=_MIB),
        boot_size=_layout_integer(record, "boot_size", alignment=_MIB),
        root_offset=_layout_integer(record, "root_offset", alignment=_MIB),
        root_size=_layout_integer(record, "root_size", alignment=_MIB),
        boot_sha256=_layout_digest(record, "boot_sha256"),
        root_sha256=_layout_digest(record, "root_sha256"),
        fit_filename=_FIT_FILENAME,
        fit_ramdisk_offset=_layout_integer(record, "fit_ramdisk_offset", alignment=4),
        fit_ramdisk_size=_layout_integer(record, "fit_ramdisk_size", alignment=4),
        fit_hash_offset=_layout_integer(record, "fit_hash_offset", alignment=4),
    )
    if layout.boot_offset + layout.boot_size != layout.root_offset:
        raise CardMediaError("Card layout must place ext4 immediately after FAT")
    if layout.capacity_bytes > 0x100000000:
        raise CardMediaError("Card layout exceeds the supported system image size")
    if (
        record.get("fit_filename") != _FIT_FILENAME
        or layout.fit_ramdisk_size != CARD_FIT_RAMDISK_SIZE
    ):
        raise CardMediaError("Card layout has an unsupported FIT filename or ramdisk size")
    return layout


def _is_inline_property(fit: bytes, offset: int, size: int) -> bool:
    return (
        _FDT_HEADER_SIZE + _FDT_PROPERTY_HEADER_SIZE <= offset <= len(fit) - size
        and offset % 4 == 0
        and struct.unpack_from(">II", fit, offset - _FDT_PROPERTY_HEADER_SIZE)
        == (_FDT_PROPERTY_TOKEN, size)
    )


def _verify_fit_slot(fit: bytes, ramdisk: bytes, data_offset: int, hash_offset: int) -> None:
    digest = hashlib.sha256(ramdisk).digest()
    data_end = data_offset + len(ramdisk)
    hash_end = hash_offset + len(digest)
    if (
        fit[:8] != struct.pack(">II", _FDT_MAGIC, len(fit))
        or not _is_inline_property(fit, data_offset, len(ramdisk))
        or not _is_inline_property(fit, hash_offset, len(digest))
        or fit[data_offset:data_end] != ramdisk
        or fit[hash_offset:hash_end] != digest
        or (data_offset < hash_end and hash_offset < data_end)
    ):
        raise CardMediaError("Invalid inline ramdisk or SHA-256 in the release FIT")


def _unique_offset(data: bytes, value: bytes) -> int:
    offset = data.find(value)
    if offset < 0 or data.find(value, offset + 1) >= 0:
        raise CardMediaError("Release FIT ramdisk or SHA-256 has no unique byte range")
    return offset


def locate_fit_ramdisk(fit: bytes, ramdisk: bytes) -> tuple[int, int]:
    """Locate the ramdisk and SHA-256 in the already verified build output."""
    data_offset = _unique_offset(fit, ramdisk)
    hash_offset = _unique_offset(fit, hashlib.sha256(ramdisk).digest())
    _verify_fit_slot(fit, ramdisk, data_offset, hash_offset)
    return data_offset, hash_offset


def _personalize_fit(
    template: bytes, layout: CardLayout, firmware: Mapping[str, Sequence[FirmwareInput]]
) -> bytes:
    data_offset, hash_offset = layout.fit_ramdisk_offset, layout.fit_hash_offset
    slot_size = layout.fit_ramdisk_size
    empty = create_device_data_initramfs({}).ljust(slot_size, b"\0")
    _verify_fit_slot(template, empty, data_offset, hash_offset)
    audio = create_device_data_initramfs({"audio-profile": firmware.get("audio-profile", ())})
    if len(audio) > slot_size:
        raise CardMediaError("Prepared audio firmware exceeds the release FIT ramdisk slot")
    ramdisk = audio.ljust(slot_size, b"\0")
    digest = hashlib.sha256(ramdisk).digest()
    fit = bytearray(template)
    fit[data_offset : data_offset + slot_size] = ramdisk
    fit[hash_offset : hash_offset + len(digest)] = digest
    return bytes(fit)


def _write_board_checksums(archive: Path, records: TextIO) -> None:
    try:
        with tarfile.open(archive, "r:") as board:
            for member in board:
                if not member.isfile():
                    continue
                contents = board.extractfile(member)
                if contents is None:
                    raise CardMediaError(f"Cannot read board archive file {member.name}")
                with contents:
                    digest = sha256_bytes(contents.read())
                records.write(f"{digest}  {member.name}\n")
    except tarfile.TarError as error:
        raise CardMediaError(f"Cannot read the board archive: {error}") from error


def _write_personal_archive(
    archive: Path, records: TextIO, firmware: Mapping[str, Sequence[FirmwareInput]]
) -> None:
    entries: dict[str, bytes] = {}
    for item in rootfs_firmware_inputs(firmware):
        name, contents = firmware_storage(item)
        path = f"lib/firmware/{name}"
        if path in entries:
            raise CardMediaError(f"Firmware destination already exists: {path}")
        entries[path] = contents
    with tarfile.open(archive, "x", format=tarfile.USTAR_FORMAT) as output:
        for path, contents in sorted(entries.items()):
            member = tarfile.TarInfo(path)
            member.mode = 0o600
            member.size = len(contents)
            output.addfile(member, BytesIO(contents))
            records.write(f"{sha256_bytes(contents)}  {path}\n")


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
        _write_board_checksums(card_dir / "board.tar", records)
        _write_personal_archive(archive, records, firmware)
    return archive, checksums, fit

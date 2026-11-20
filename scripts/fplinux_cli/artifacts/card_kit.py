# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- card kit failures use exact diagnostics.
"""Describe the release card payload and validate its inline FIT slot."""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

from fplinux_cli.common import is_sha256, read_json_object

if TYPE_CHECKING:
    from collections.abc import Iterable
    from pathlib import Path

CARD_APK_INDEX = "apks/Packages.adb"
CARD_APK_KEY = "apks/fplinux-build.rsa.pub"
CARD_APK_STATE = "apk-state.tar"
CARD_MEMBERS = (
    "card/boot.xz",
    "card/root.ext4.xz",
    "card/board.tar",
    "card/fit.itb",
    "card/layout.json",
    f"card/{CARD_APK_STATE}",
    f"card/{CARD_APK_INDEX}",
    f"card/{CARD_APK_KEY}",
)
FIT_AUDIO_CPIO_BYTES = 4096

_MIB = 1024 * 1024
_FDT_MAGIC = 0xD00DFEED
_FDT_HEADER_SIZE = 40
_FDT_PROPERTY_TOKEN = 3
_FDT_PROPERTY_HEADER_SIZE = 12
_FIT_FILENAME = "FPLINUX.ITB"


class CardKitError(RuntimeError):
    """A release card kit or its personalized contents are invalid."""


def card_package_members(paths: Iterable[str]) -> tuple[str, ...]:
    """Read the exact optional APK members admitted by the card build."""
    return tuple(
        sorted(
            name
            for name in paths
            if name.startswith("card/apks/") and name.endswith(".apk") and name.count("/") == 2
        )
    )


@dataclass(frozen=True)
class CardLayout:
    """The target's fixed FAT and ext4 ranges, raw digests and inline FIT slot."""

    target: str
    release_id: str
    boot_offset: int
    boot_size: int
    root_offset: int
    root_size: int
    disk_signature: int
    boot_sha256: str
    root_sha256: str
    fit_ramdisk_offset: int
    fit_ramdisk_size: int
    fit_hash_offset: int

    @property
    def capacity_bytes(self) -> int:
        """Return the minimum card capacity for the complete layout."""
        return self.root_offset + self.root_size


def _layout_integer(record: dict[str, object], field: str, *, alignment: int) -> int:
    value = record.get(field)
    if type(value) is not int or value <= 0 or value % alignment:
        raise CardKitError(f"Card layout has an invalid {field}")
    return value


def _layout_digest(record: dict[str, object], field: str) -> str:
    value = record.get(field)
    if not isinstance(value, str) or not is_sha256(value):
        raise CardKitError(f"Card layout has an invalid {field}")
    return value


def read_card_layout(card_dir: Path) -> CardLayout:
    """Read the byte ranges and identities from the admitted kit's layout."""
    record = read_json_object(card_dir / "layout.json")
    if record is None:
        raise CardKitError("Cannot read the release card layout")
    target = record.get("target")
    release_id = record.get("release_id")
    if (
        not isinstance(target, str)
        or not target
        or not isinstance(release_id, str)
        or not release_id
    ):
        raise CardKitError("Card layout lacks a phone model or release identity")
    disk_signature = record.get("disk_signature")
    if type(disk_signature) is not int or not 0 <= disk_signature <= 0xFFFFFFFF:
        raise CardKitError("Card layout has an invalid disk_signature")
    layout = CardLayout(
        target=target,
        release_id=release_id,
        boot_offset=_layout_integer(record, "boot_offset", alignment=_MIB),
        boot_size=_layout_integer(record, "boot_size", alignment=_MIB),
        root_offset=_layout_integer(record, "root_offset", alignment=_MIB),
        root_size=_layout_integer(record, "root_size", alignment=_MIB),
        disk_signature=disk_signature,
        boot_sha256=_layout_digest(record, "boot_sha256"),
        root_sha256=_layout_digest(record, "root_sha256"),
        fit_ramdisk_offset=_layout_integer(record, "fit_ramdisk_offset", alignment=4),
        fit_ramdisk_size=_layout_integer(record, "fit_ramdisk_size", alignment=512),
        fit_hash_offset=_layout_integer(record, "fit_hash_offset", alignment=4),
    )
    if layout.boot_offset + layout.boot_size != layout.root_offset:
        raise CardKitError("Card layout must place ext4 immediately after FAT")
    if layout.capacity_bytes > 0x100000000:
        raise CardKitError("Card layout exceeds the supported system image size")
    if (
        record.get("fit_filename") != _FIT_FILENAME
        or layout.fit_ramdisk_size <= FIT_AUDIO_CPIO_BYTES
    ):
        raise CardKitError("Card layout has an unsupported FIT filename or ramdisk size")
    return layout


def _is_inline_property(fit: bytes, offset: int, size: int) -> bool:
    return (
        _FDT_HEADER_SIZE + _FDT_PROPERTY_HEADER_SIZE <= offset <= len(fit) - size
        and offset % 4 == 0
        and struct.unpack_from(">II", fit, offset - _FDT_PROPERTY_HEADER_SIZE)
        == (_FDT_PROPERTY_TOKEN, size)
    )


def verify_fit_slot(fit: bytes, ramdisk: bytes, data_offset: int, hash_offset: int) -> None:
    """Check the exact inline ramdisk and its SHA-256 property before editing the FIT."""
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
        raise CardKitError("Invalid inline ramdisk or SHA-256 in the release FIT")


def _unique_offset(data: bytes, value: bytes) -> int:
    offset = data.find(value)
    if offset < 0 or data.find(value, offset + 1) >= 0:
        raise CardKitError("Release FIT ramdisk or SHA-256 has no unique byte range")
    return offset


def locate_fit_ramdisk(fit: bytes, ramdisk: bytes) -> tuple[int, int]:
    """Locate the ramdisk and SHA-256 in the already verified build output."""
    data_offset = _unique_offset(fit, ramdisk)
    hash_offset = _unique_offset(fit, hashlib.sha256(ramdisk).digest())
    verify_fit_slot(fit, ramdisk, data_offset, hash_offset)
    return data_offset, hash_offset

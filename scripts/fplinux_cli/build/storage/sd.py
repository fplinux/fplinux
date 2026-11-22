# SPDX-License-Identifier: GPL-2.0-only
"""Assemble one deterministic microSD image from prebuilt artifacts."""

from __future__ import annotations

import hashlib
import lzma
import os
import shutil
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.build import environment as build_env

from . import layout as profile_layout

if TYPE_CHECKING:
    from collections.abc import Mapping
    from typing import BinaryIO

    from fplinux_cli.build.bootstrap.result import ColdSdBootstrap

_COMMAND_TIMEOUT_SECONDS = 120
_COPY_CHUNK_BYTES = 1024 * 1024
_SECTOR_BYTES = 512


class SdImageError(RuntimeError):
    """Raised when one microSD input or output is invalid."""


@dataclass(frozen=True)
class BootPrefix:
    """Written boot bytes and their digest, independent of FAT capacity."""

    size: int
    sha256: str


def _require_input(path: Path, name: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise SdImageError(f"{name} is missing or invalid: {path}")
    if path.stat().st_size == 0:
        raise SdImageError(f"{name} is empty: {path}")


def compressed_image_name(storage: Mapping[str, object]) -> str:
    """Return the public compressed-image name from the profile storage contract."""
    filename = storage.get("filename")
    if not isinstance(filename, str) or Path(filename).name != filename:
        message = "microSD image filename is invalid"
        raise SdImageError(message)
    return f"{filename}.xz"


def _require_integer(value: object, name: str) -> int:
    if type(value) is not int:
        raise SdImageError(f"microSD {name} is invalid")
    return value


def _run(command: list[str], *, stdout: BinaryIO | None = None) -> None:
    try:
        result = subprocess.run(
            command,
            stdout=stdout,
            stderr=subprocess.PIPE,
            text=stdout is None,
            env=build_env.build_environment(),
            check=False,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise SdImageError(f"cannot run {command[0]}: {error}") from error
    if result.returncode:
        detail = result.stderr.strip() if isinstance(result.stderr, str) else ""
        raise SdImageError(detail or f"{command[0]} exited with {result.returncode}")


def build(
    rootfs: Path,
    destination: Path,
    *,
    storage: Mapping[str, object],
    cold_boot: ColdSdBootstrap | None = None,
) -> Path:
    """Publish one ``FPLINUX.img.xz`` from ext4 and its optional autonomous entry."""
    compressed_name = compressed_image_name(storage)
    if destination.name != compressed_name:
        raise SdImageError(f"microSD image must be named {compressed_name}")
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        raise SdImageError(f"microSD output is invalid: {destination}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        dir=destination.parent, prefix=".fplinux-sd-image."
    ) as temporary_name:
        temporary = Path(temporary_name)
        raw, _ = _assemble(temporary, rootfs, storage=storage, cold_boot=cold_boot)
        staging = temporary / compressed_name
        with staging.open("wb") as stream:
            _run(["xz", "--threads=1", "--check=crc64", "--stdout", str(raw)], stdout=stream)
        if staging.stat().st_size == 0:
            message = f"xz did not produce {compressed_name}"
            raise SdImageError(message)
        staging.replace(destination)
    return destination


def write_boot_prefix(
    rootfs: Path,
    destination: Path,
    *,
    storage: Mapping[str, object],
    cold_boot: ColdSdBootstrap,
) -> BootPrefix:
    """Pack the MBR, ROM reader, FAT metadata and all allocated FAT clusters."""
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".card-boot.") as name:
        raw, prefix_size = _assemble(Path(name), rootfs, storage=storage, cold_boot=cold_boot)
        return BootPrefix(prefix_size, compress_card_region(raw, destination, prefix_size))


def _allocated_fat_size(image: Path, boot_size: int) -> int:
    # The first FAT copy describes the full allocation, including clusters of zero bytes.
    # Keep every FAT copy and the whole final allocated cluster, then round up for the writer.
    with image.open("rb") as stream:
        sector = stream.read(_SECTOR_BYTES)
        sector_bytes, cluster_sectors, reserved_sectors, fat_copies = struct.unpack_from(
            "<HBHB", sector, 11
        )
        total_sectors, fat_sectors = struct.unpack_from("<II", sector, 32)
        data_start = reserved_sectors + fat_copies * fat_sectors
        if (
            sector_bytes != _SECTOR_BYTES
            or total_sectors * _SECTOR_BYTES != boot_size
            or cluster_sectors == 0
            or reserved_sectors == 0
            or fat_copies != 2
            or fat_sectors == 0
            or data_start >= total_sectors
        ):
            message = "generated FAT32 geometry differs from the profile layout"
            raise SdImageError(message)
        stream.seek(reserved_sectors * _SECTOR_BYTES)
        fat = stream.read(fat_sectors * _SECTOR_BYTES)
    last_used = (len(fat.rstrip(b"\0")) + 3) // 4 - 1
    root_cluster = struct.unpack_from("<I", sector, 44)[0]
    data_end = data_start + (last_used - 1) * cluster_sectors
    if not 2 <= root_cluster <= last_used or data_end > total_sectors:
        message = "generated FAT32 cluster allocation is invalid"
        raise SdImageError(message)
    end = data_end * _SECTOR_BYTES
    return (end + _COPY_CHUNK_BYTES - 1) // _COPY_CHUNK_BYTES * _COPY_CHUNK_BYTES


def _stage_fat(
    image: Path,
    *,
    storage: Mapping[str, object],
    cold_boot: ColdSdBootstrap | None,
) -> int:
    boot_size = _require_integer(storage["boot_size"], "boot size")
    disk_signature = _require_integer(storage["disk_signature"], "disk signature")
    with image.open("wb") as stream:
        stream.truncate(boot_size)
    _run(
        [
            "mkdosfs",
            "-F",
            "32",
            "-a",
            "--invariant",
            "-i",
            f"{disk_signature:08x}",
            "-n",
            str(storage["boot_label"]),
            str(image),
        ]
    )
    epoch = int(build_env.SOURCE_DATE_EPOCH)
    shared = image.parent / "Shared"
    shared.mkdir()
    os.utime(shared, (epoch, epoch))
    directories = []
    if cold_boot is not None:
        _require_input(cold_boot.stage0, "autonomous microSD stage0")
        boot_files = image.parent / "fpbin_t117"
        boot_files.mkdir()
        stage0 = boot_files / "fpmain.bin"
        shutil.copyfile(cold_boot.stage0, stage0)
        os.utime(stage0, (epoch, epoch))
        os.utime(boot_files, (epoch, epoch))
        directories.append(str(boot_files))
    directories.append(str(shared))
    _run(["mcopy", "-sm", "-i", str(image), *directories, "::"])
    allocated_size = _allocated_fat_size(image, boot_size)
    # The declared MBR extent retains the full FAT capacity. Copy its initialized bytes
    # explicitly so assembly is bounded even on filesystems without hole mapping.
    with image.open("r+b") as stream:
        stream.truncate(allocated_size)
    return allocated_size


def compress_card_region(source: Path, destination: Path, size: int) -> str:
    """Compress the exact leading bytes for the phone's bounded XZ decoder."""
    digest = hashlib.sha256()
    # BusyBox uses the embedded decoder. Preset 4 needs a 4 MiB dictionary.
    with (
        source.open("rb") as input_stream,
        lzma.open(destination, "wb", check=lzma.CHECK_CRC32, preset=4) as compressed,
    ):
        remaining = size
        while remaining:
            chunk = input_stream.read(min(remaining, _COPY_CHUNK_BYTES))
            if not chunk:
                raise SdImageError(f"microSD region ends before {size} bytes: {source.name}")
            digest.update(chunk)
            compressed.write(chunk)
            remaining -= len(chunk)
    return digest.hexdigest()


def _assemble(
    temporary: Path,
    rootfs: Path,
    *,
    storage: Mapping[str, object],
    cold_boot: ColdSdBootstrap | None,
) -> tuple[Path, int]:
    image_name = str(storage["filename"])
    rootfs_name = str(storage["root_filename"])
    rootfs_bytes = _require_integer(storage["root_size"], "root size")
    _require_input(rootfs, rootfs_name)
    if rootfs.name != rootfs_name:
        raise SdImageError(f"root filesystem input must be named {rootfs_name}")
    if rootfs.stat().st_size != rootfs_bytes:
        raise SdImageError(f"{rootfs_name} must be exactly {rootfs_bytes} bytes")
    boot_offset = _require_integer(storage["boot_offset"], "boot offset")
    input_path = temporary / "input"
    output_path = temporary / "output"
    root_path = temporary / "root"
    tool_temporary = temporary / "tmp"
    config = temporary / "genimage.cfg"
    staged_rootfs = input_path / rootfs_name
    for directory in (input_path, output_path, root_path, tool_temporary):
        directory.mkdir()
    config.write_bytes(profile_layout.genimage_config(storage))
    fat_size = _stage_fat(
        input_path / profile_layout.BOOT_IMAGE_NAME, storage=storage, cold_boot=cold_boot
    )
    shutil.copyfile(rootfs, staged_rootfs)
    _require_input(staged_rootfs, rootfs_name)
    _run(
        [
            "genimage",
            "--rootpath",
            str(root_path),
            "--tmppath",
            str(tool_temporary),
            "--inputpath",
            str(input_path),
            "--outputpath",
            str(output_path),
            "--config",
            str(config),
        ]
    )
    raw = output_path / image_name
    if raw.is_symlink() or not raw.is_file() or raw.stat().st_size == 0:
        message = f"genimage did not produce {image_name}"
        raise SdImageError(message)
    expected_raw_bytes = _require_integer(storage["root_offset"], "root offset") + rootfs_bytes
    if raw.stat().st_size != expected_raw_bytes:
        raise SdImageError(f"{image_name} size differs from the profile layout")
    if cold_boot is not None:
        _install_card_reader(raw, cold_boot, boot_offset)
        extracted = temporary / "checked-stage0.bin"
        _run(
            [
                "mcopy",
                "-i",
                f"{raw}@@{boot_offset}",
                "::fpbin_t117/fpmain.bin",
                str(extracted),
            ]
        )
        if extracted.read_bytes() != cold_boot.stage0.read_bytes():
            message = "autonomous entry differs from the file written to FAT"
            raise SdImageError(message)
    return raw, boot_offset + fat_size


def _install_card_reader(image: Path, cold_boot: ColdSdBootstrap, boot_offset: int) -> None:
    """Place the ROM's reader ahead of partition 1 while retaining the MBR table."""
    _require_input(cold_boot.reader, "autonomous microSD reader")
    reader = cold_boot.reader.read_bytes()
    if not 512 < len(reader) <= boot_offset:
        message = "autonomous reader must fit between the MBR and the first partition"
        raise SdImageError(message)
    with image.open("r+b") as stream:
        stream.seek(440)
        partition_table = stream.read(72)
        stream.seek(0)
        stream.write(reader[:440] + partition_table + reader[512:])

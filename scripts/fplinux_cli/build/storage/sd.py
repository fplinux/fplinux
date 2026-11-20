# SPDX-License-Identifier: GPL-2.0-only
"""Assemble one deterministic microSD image from prebuilt artifacts."""

from __future__ import annotations

import hashlib
import lzma
import os
import shutil
import subprocess
import tempfile
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


class SdImageError(RuntimeError):
    """Raised when one microSD input or output is invalid."""


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
        raw = _assemble(temporary, rootfs, storage=storage, cold_boot=cold_boot)
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
) -> str:
    """Pack the fixed card prefix with the autonomous FAT entry and return its raw digest."""
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".card-boot.") as name:
        raw = _assemble(Path(name), rootfs, storage=storage, cold_boot=cold_boot)
        return compress_card_region(
            raw, destination, _require_integer(storage["root_offset"], "root offset")
        )


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
) -> Path:
    image_name = str(storage["filename"])
    rootfs_name = str(storage["root_filename"])
    rootfs_bytes = _require_integer(storage["root_size"], "root size")
    _require_input(rootfs, rootfs_name)
    if rootfs.name != rootfs_name:
        raise SdImageError(f"root filesystem input must be named {rootfs_name}")
    if rootfs.stat().st_size != rootfs_bytes:
        raise SdImageError(f"{rootfs_name} must be exactly {rootfs_bytes} bytes")
    epoch = int(build_env.SOURCE_DATE_EPOCH)
    input_path = temporary / "input"
    output_path = temporary / "output"
    root_path = temporary / "root"
    tool_temporary = temporary / "tmp"
    config = temporary / "genimage.cfg"
    staged_rootfs = input_path / rootfs_name
    for directory in (input_path, output_path, root_path, tool_temporary):
        directory.mkdir()
    config.write_bytes(profile_layout.genimage_config(storage, cold_boot=cold_boot is not None))
    if cold_boot is not None:
        _require_input(cold_boot.stage0, "autonomous microSD stage0")
        stage0 = input_path / "cold-stage0.bin"
        shutil.copyfile(cold_boot.stage0, stage0)
        os.utime(stage0, (epoch, epoch))
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
        _install_card_reader(
            raw, cold_boot, _require_integer(storage["boot_offset"], "boot offset")
        )
        extracted = temporary / "checked-stage0.bin"
        _run(
            [
                "mcopy",
                "-i",
                f"{raw}@@{storage['boot_offset']}",
                "::fpbin_t117/fpmain.bin",
                str(extracted),
            ]
        )
        if extracted.read_bytes() != cold_boot.stage0.read_bytes():
            message = "autonomous entry differs from the file written to FAT"
            raise SdImageError(message)
    return raw


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

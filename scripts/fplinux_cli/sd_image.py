# SPDX-License-Identifier: GPL-2.0-only
"""Assemble one deterministic TA-1618 microSD image from prebuilt artifacts."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from . import build_env, profile_layout

if TYPE_CHECKING:
    from collections.abc import Mapping
    from typing import BinaryIO

_COMMAND_TIMEOUT_SECONDS = 120


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
    fit: Path,
    rootfs: Path,
    destination: Path,
    *,
    fit_spec: Mapping[str, object],
    storage: Mapping[str, object],
) -> Path:
    """Publish one ``FPLINUX.img.xz`` from exact FIT and ext4 inputs."""
    compressed_name = compressed_image_name(storage)
    image_name = str(storage["filename"])
    fit_name = str(fit_spec["filename"])
    rootfs_name = str(storage["root_filename"])
    rootfs_bytes = _require_integer(storage["root_size"], "root size")
    if destination.name != compressed_name:
        raise SdImageError(f"microSD image must be named {compressed_name}")
    _require_input(fit, fit_name)
    _require_input(rootfs, rootfs_name)
    if fit.name != fit_name:
        raise SdImageError(f"FIT input must be named {fit_name}")
    if rootfs.name != rootfs_name:
        raise SdImageError(f"root filesystem input must be named {rootfs_name}")
    if rootfs.stat().st_size != rootfs_bytes:
        raise SdImageError(f"{rootfs_name} must be exactly {rootfs_bytes} bytes")
    if destination.is_symlink() or (destination.exists() and not destination.is_file()):
        raise SdImageError(f"microSD output is invalid: {destination}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    epoch = int(build_env.SOURCE_DATE_EPOCH)
    with tempfile.TemporaryDirectory(
        dir=destination.parent, prefix=".fplinux-sd-image."
    ) as temporary_name:
        temporary = Path(temporary_name)
        input_path = temporary / "input"
        output_path = temporary / "output"
        root_path = temporary / "root"
        tool_temporary = temporary / "tmp"
        staging = temporary / compressed_name
        config = temporary / "genimage.cfg"
        staged_fit = input_path / fit_name
        staged_rootfs = input_path / rootfs_name
        input_path.mkdir()
        output_path.mkdir()
        root_path.mkdir()
        tool_temporary.mkdir()
        config.write_bytes(profile_layout.genimage_config(fit_spec, storage))

        shutil.copyfile(fit, staged_fit)
        shutil.copyfile(rootfs, staged_rootfs)
        os.utime(staged_fit, (epoch, epoch))
        _require_input(staged_fit, fit_name)
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
        with staging.open("wb") as stream:
            _run(["xz", "--threads=1", "--check=crc64", "--stdout", str(raw)], stdout=stream)
        if staging.stat().st_size == 0:
            message = f"xz did not produce {compressed_name}"
            raise SdImageError(message)
        staging.replace(destination)
    return destination

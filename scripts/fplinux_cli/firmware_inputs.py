# SPDX-License-Identifier: GPL-2.0-only
"""Capture and install explicitly declared local rootfs firmware inputs."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from collections.abc import Sequence

FIRMWARE_SNAPSHOT_ROOT = PurePosixPath(".fplinux-inputs/firmware")


def _fail(message: str) -> NoReturn:
    raise SystemExit(f"firmware input error: {message}")


@dataclass(frozen=True)
class FirmwareInput:
    """One admitted input whose bytes remain fixed for this build execution."""

    source: str
    destination: str
    contents: bytes
    sha256: str

    @property
    def size(self) -> int:
        """Return the admitted byte count."""
        return len(self.contents)

    def recipe_record(self) -> dict[str, int | str]:
        """Describe the input without embedding its private contents in a receipt."""
        return {
            "source": self.source,
            "destination": self.destination,
            "size": self.size,
            "sha256": self.sha256,
        }


def external_firmware_directory(cache: Path, target: str) -> Path:
    """Return the sole host directory accepted for one target's local inputs."""
    return cache / "firmware" / target


def snapshot_firmware_directory(root: Path, target: str) -> Path:
    """Return the non-cache workspace location used inside a build container."""
    return root / FIRMWARE_SNAPSHOT_ROOT / target


def snapshot_firmware_path(target: str, source: str) -> str:
    """Return one captured input's immutable workspace-relative path."""
    return (FIRMWARE_SNAPSHOT_ROOT / target / source).as_posix()


def capture_external_firmware_inputs(
    target: str,
    declarations: Sequence[dict[str, Any]],
    cache: Path,
) -> tuple[FirmwareInput, ...]:
    """Capture one optional all-or-nothing group from the ignored host directory."""
    if not declarations:
        return ()
    directory = external_firmware_directory(cache, target)
    return _capture_optional_firmware_group(
        declarations,
        directory,
        (cache, cache / "firmware", directory),
    )


def capture_snapshot_firmware_inputs(
    target: str,
    declarations: Sequence[dict[str, Any]],
    root: Path,
) -> tuple[FirmwareInput, ...]:
    """Read one optional all-or-nothing group from an immutable build snapshot."""
    if not declarations:
        return ()
    input_root = root / FIRMWARE_SNAPSHOT_ROOT
    directory = snapshot_firmware_directory(root, target)
    return _capture_optional_firmware_group(
        declarations,
        directory,
        (root, root / ".fplinux-inputs", input_root, directory),
    )


def _capture_optional_firmware_group(
    declarations: Sequence[dict[str, Any]],
    directory: Path,
    directory_chain: Sequence[Path],
) -> tuple[FirmwareInput, ...]:
    """Accept no declared files or require the complete declared generation."""
    existing_chain = [path for path in directory_chain if path.exists() or path.is_symlink()]
    for path in existing_chain:
        if path.is_symlink() or not path.is_dir():
            _fail(f"declared input directory is missing or invalid: {path}")
    if not directory.exists():
        return ()

    any_declared_file = any(
        (directory / str(declaration["source"])).exists()
        or (directory / str(declaration["source"])).is_symlink()
        for declaration in declarations
    )
    if not any_declared_file:
        return ()
    _require_directory_chain(directory_chain)
    return capture_firmware_inputs(declarations, directory)


def capture_firmware_inputs(
    declarations: Sequence[dict[str, Any]], directory: Path
) -> tuple[FirmwareInput, ...]:
    """Read and admit every declaration from one exact input directory."""
    if not declarations:
        return ()
    if directory.is_symlink() or not directory.is_dir():
        _fail(f"declared input directory is missing or invalid: {directory}")

    captured: list[FirmwareInput] = []
    for declaration in declarations:
        source_name = str(declaration["source"])
        source = directory / source_name
        if source.is_symlink() or not source.is_file():
            _fail(f"declared file is missing or invalid: {source}")
        try:
            contents = source.read_bytes()
        except OSError as error:
            _fail(f"declared file cannot be read: {source}: {error}")

        declared_size = int(declaration["size"])
        if len(contents) != declared_size:
            _fail(f"{source_name} has {len(contents)} bytes; expected {declared_size}")
        actual_sha256 = hashlib.sha256(contents).hexdigest()
        expected_sha256 = declaration.get("sha256")
        if expected_sha256 is not None and actual_sha256 != expected_sha256:
            _fail(f"{source_name} SHA-256 is {actual_sha256}; expected {expected_sha256}")
        captured.append(
            FirmwareInput(
                source=source_name,
                destination=str(declaration["destination"]),
                contents=contents,
                sha256=actual_sha256,
            )
        )
    return tuple(captured)


def install_firmware_inputs(root: Path, inputs: Sequence[FirmwareInput]) -> None:
    """Install admitted bytes under /lib/firmware with private file permissions."""
    if not inputs:
        return
    if root.is_symlink() or not root.is_dir():
        _fail(f"root filesystem is missing or invalid: {root}")

    firmware_root = _require_or_create_directory(root / "lib", "rootfs /lib") / "firmware"
    firmware_root = _require_or_create_directory(firmware_root, "rootfs /lib/firmware")
    for firmware in inputs:
        destination = firmware_root
        parts = PurePosixPath(firmware.destination).parts
        for part in parts[:-1]:
            destination = _require_or_create_directory(
                destination / part, f"firmware destination parent for {firmware.destination}"
            )
        destination /= parts[-1]
        if destination.exists() or destination.is_symlink():
            _fail(f"firmware destination already exists: {destination}")
        try:
            written = destination.write_bytes(firmware.contents)
            if written != firmware.size:
                _fail(f"firmware destination was not written completely: {destination}")
            destination.chmod(0o600)
        except OSError as error:
            _fail(f"firmware destination cannot be installed: {destination}: {error}")


def verify_installed_firmware_inputs(root: Path, inputs: Sequence[FirmwareInput]) -> None:
    """Require the composed rootfs to contain the exact admitted bytes and mode."""
    for firmware in inputs:
        destination = root / "lib/firmware" / firmware.destination
        if destination.is_symlink() or not destination.is_file():
            _fail(f"installed firmware is missing or invalid: {destination}")
        try:
            contents = destination.read_bytes()
            mode = destination.stat().st_mode & 0o777
        except OSError as error:
            _fail(f"installed firmware cannot be verified: {destination}: {error}")
        if contents != firmware.contents:
            _fail(f"installed firmware bytes do not match the captured input: {destination}")
        if mode != 0o600:
            _fail(f"installed firmware mode is {mode:04o}, expected 0600: {destination}")


def _require_or_create_directory(path: Path, name: str) -> Path:
    """Create one destination directory without accepting a symlink component."""
    try:
        path.mkdir(mode=0o755)
    except FileExistsError:
        pass
    except OSError as error:
        _fail(f"{name} cannot be created: {path}: {error}")
    if path.is_symlink() or not path.is_dir():
        _fail(f"{name} is missing or invalid: {path}")
    return path


def _require_directory_chain(paths: Sequence[Path]) -> None:
    """Reject a missing or linked component in a fixed input directory."""
    for path in paths:
        if path.is_symlink() or not path.is_dir():
            _fail(f"declared input directory is missing or invalid: {path}")

# SPDX-License-Identifier: GPL-2.0-only
"""Capture declared device-data groups for rootfs and built-in firmware delivery."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from .common import fail, sha256_bytes

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

DEVICE_DATA_SNAPSHOT_ROOT = PurePosixPath(".fplinux-inputs/device-data")
GENERATION_NAME = re.compile(r"generation-[A-Za-z0-9_-]+")


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


def device_data_cache_directory(cache: Path, target: str) -> Path:
    """Return the private generation root used by one target."""
    return cache / "device-data" / target


def snapshot_device_data_group_directory(root: Path, target: str, group: str) -> Path:
    """Return one group's immutable workspace directory inside a build container."""
    return root / DEVICE_DATA_SNAPSHOT_ROOT / target / "groups" / group


def snapshot_device_data_path(target: str, group: str, destination: str) -> str:
    """Return one captured input's immutable workspace-relative destination path."""
    return (DEVICE_DATA_SNAPSHOT_ROOT / target / "groups" / group / destination).as_posix()


def current_device_data_generation(cache: Path, target: str) -> Path | None:
    """Resolve the atomically selected generation; an old or absent layout is a miss."""
    directory = device_data_cache_directory(cache, target)
    if directory.is_symlink():
        fail(f"device-data directory is invalid: {directory}")
    if not directory.exists():
        return None
    if not directory.is_dir():
        fail(f"device-data directory is invalid: {directory}")
    pointer = directory / "current"
    if pointer.is_symlink():
        fail(f"device-data current pointer is invalid: {pointer}")
    if not pointer.exists():
        return None
    if not pointer.is_file():
        fail(f"device-data current pointer is invalid: {pointer}")
    try:
        value = pointer.read_text(encoding="ascii")
    except (OSError, UnicodeError) as error:
        fail(f"device-data current pointer cannot be read: {pointer}: {error}")
    if not value.endswith("\n") or value.count("\n") != 1:
        fail(f"device-data current pointer is invalid: {pointer}")
    generation_name = value[:-1]
    if GENERATION_NAME.fullmatch(generation_name) is None:
        fail(f"device-data current pointer is invalid: {pointer}")
    generations = directory / "generations"
    generation = generations / generation_name
    _require_directory_chain((cache, cache / "device-data", directory, generations, generation))
    return generation


def capture_external_device_data(
    target: str,
    groups: Mapping[str, list[dict[str, Any]]],
    cache: Path,
) -> dict[str, tuple[FirmwareInput, ...]]:
    """Capture every complete current group while allowing whole groups to be absent."""
    if not groups:
        return {}
    generation = current_device_data_generation(cache, target)
    if generation is None:
        return {}
    return capture_device_data_generation(
        groups,
        generation,
        path_field="source",
        require_all=False,
    )


def capture_snapshot_device_data(
    target: str,
    groups: Mapping[str, list[dict[str, Any]]],
    root: Path,
) -> dict[str, tuple[FirmwareInput, ...]]:
    """Read all complete groups from one immutable build workspace snapshot."""
    if not groups:
        return {}
    generation = root / DEVICE_DATA_SNAPSHOT_ROOT / target
    if not generation.exists():
        return {}
    return capture_device_data_generation(
        groups,
        generation,
        path_field="destination",
        require_all=False,
    )


def capture_device_data_generation(
    groups: Mapping[str, list[dict[str, Any]]],
    generation: Path,
    *,
    path_field: str,
    require_all: bool,
) -> dict[str, tuple[FirmwareInput, ...]]:
    """Validate named all-or-nothing groups from one staged or selected generation."""
    _require_directory_chain((generation, generation / "groups"))
    captured: dict[str, tuple[FirmwareInput, ...]] = {}
    for group_name in sorted(groups):
        declarations = groups[group_name]
        directory = generation / "groups" / group_name
        paths = [directory / str(declaration[path_field]) for declaration in declarations]
        present = any(path.exists() or path.is_symlink() for path in paths)
        if not present:
            if require_all:
                fail(f"device-data group {group_name} is absent")
            continue
        if directory.is_symlink() or not directory.is_dir():
            fail(f"device-data group {group_name} directory is missing or invalid: {directory}")
        captured[group_name] = _capture_group_firmware(
            declarations,
            directory,
            path_field=path_field,
            group=group_name,
        )
    return captured


def _capture_group_firmware(
    declarations: Sequence[dict[str, Any]],
    directory: Path,
    *,
    path_field: str,
    group: str,
) -> tuple[FirmwareInput, ...]:
    """Read and admit every declaration from one exact input directory."""
    captured: list[FirmwareInput] = []
    for declaration in declarations:
        source_name = str(declaration["source"])
        relative = str(declaration[path_field])
        source = directory / relative
        prefix = f"device-data group {group}: "
        _require_input_file(source, directory, prefix)
        try:
            contents = source.read_bytes()
        except OSError as error:
            fail(f"{prefix}declared file cannot be read: {source}: {error}")

        declared_size = int(declaration["size"])
        if len(contents) != declared_size:
            fail(f"{prefix}{source_name} has {len(contents)} bytes; expected {declared_size}")
        actual_sha256 = sha256_bytes(contents)
        expected_sha256 = declaration.get("sha256")
        if expected_sha256 is not None and actual_sha256 != expected_sha256:
            fail(f"{prefix}{source_name} SHA-256 is {actual_sha256}; expected {expected_sha256}")
        captured.append(
            FirmwareInput(
                source=source_name,
                destination=str(declaration["destination"]),
                contents=contents,
                sha256=actual_sha256,
            )
        )
    return tuple(captured)


def _require_input_file(path: Path, root: Path, prefix: str) -> None:
    """Reject linked parents as well as a linked or missing declared file."""
    relative = path.relative_to(root)
    current = root
    for part in relative.parts[:-1]:
        current /= part
        if current.is_symlink() or not current.is_dir():
            fail(f"{prefix}declared file parent is missing or invalid: {current}")
    if path.is_symlink() or not path.is_file():
        fail(f"{prefix}declared file is missing or invalid: {path}")


def install_firmware_inputs(root: Path, inputs: Sequence[FirmwareInput]) -> None:
    """Install admitted bytes under /lib/firmware with private file permissions."""
    if not inputs:
        return
    if root.is_symlink() or not root.is_dir():
        fail(f"root filesystem is missing or invalid: {root}")

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
            fail(f"firmware destination already exists: {destination}")
        try:
            written = destination.write_bytes(firmware.contents)
            if written != firmware.size:
                fail(f"firmware destination was not written completely: {destination}")
            destination.chmod(0o600)
        except OSError as error:
            fail(f"firmware destination cannot be installed: {destination}: {error}")


def verify_installed_firmware_inputs(root: Path, inputs: Sequence[FirmwareInput]) -> None:
    """Require the composed rootfs to contain the exact admitted bytes and mode."""
    for firmware in inputs:
        destination = root / "lib/firmware" / firmware.destination
        if destination.is_symlink() or not destination.is_file():
            fail(f"installed firmware is missing or invalid: {destination}")
        try:
            contents = destination.read_bytes()
            mode = destination.stat().st_mode & 0o777
        except OSError as error:
            fail(f"installed firmware cannot be verified: {destination}: {error}")
        if contents != firmware.contents:
            fail(f"installed firmware bytes do not match the captured input: {destination}")
        if mode != 0o600:
            fail(f"installed firmware mode is {mode:04o}, expected 0600: {destination}")


def _require_or_create_directory(path: Path, name: str) -> Path:
    """Create one destination directory without accepting a symlink component."""
    try:
        path.mkdir(mode=0o755)
    except FileExistsError:
        pass
    except OSError as error:
        fail(f"{name} cannot be created: {path}: {error}")
    if path.is_symlink() or not path.is_dir():
        fail(f"{name} is missing or invalid: {path}")
    return path


def _require_directory_chain(paths: Sequence[Path]) -> None:
    """Reject a missing or linked component in a fixed input directory."""
    for path in paths:
        if path.is_symlink() or not path.is_dir():
            fail(f"declared input directory is missing or invalid: {path}")

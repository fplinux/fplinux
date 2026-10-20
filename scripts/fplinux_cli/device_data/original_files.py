# SPDX-License-Identifier: GPL-2.0-only
"""Admit a target's complete named originals from one local manifest."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from fplinux_cli.common import fail, sha256_bytes
from fplinux_cli.manifests.values import (
    basename_value,
    exact_table,
    integer_value,
    nonempty_string,
    sha256_value,
)


@dataclass(frozen=True)
class OriginalFilesSource:
    """The exact source record and all bytes admitted by that record."""

    manifest: bytes
    files: dict[str, bytes]


def _read_original(path: Path, description: str) -> bytes:
    """Capture one regular source without following a symlink or changing it."""
    if path.is_symlink() or not path.is_file():
        fail(f"{description} is missing or invalid: {path}")
    try:
        return path.read_bytes()
    except OSError as error:
        fail(f"{description} cannot be read: {path}: {error}")


def read_original_files(target: str, manifest_path: Path) -> OriginalFilesSource:
    """Read each input once and bind its bytes to the selected target and source record.

    Relative source directories are resolved beside the manifest. Digests bind the
    supplied record; they do not establish a phone's identity or authenticate it.
    """
    raw = _read_original(manifest_path, "device-data originals manifest")
    try:
        value = json.loads(raw.decode("utf-8"))
    except ValueError as error:
        fail(f"device-data originals manifest is invalid JSON: {error}")
    manifest = exact_table(
        value, {"target", "directory", "files"}, "device-data originals manifest"
    )
    if manifest["target"] != target:
        fail(f"device-data originals manifest must select target {target}")
    directory = Path(nonempty_string(manifest["directory"], "device-data originals directory"))
    if not directory.is_absolute():
        directory = manifest_path.parent / directory
    if directory.is_symlink() or not directory.is_dir():
        fail(f"device-data originals directory is missing or invalid: {directory}")
    records = manifest["files"]
    if not isinstance(records, list) or not records:
        fail("device-data originals files must be a non-empty array")
    declared: dict[str, tuple[int, str]] = {}
    for index, raw_record in enumerate(records):
        label = f"device-data originals files[{index}]"
        record = exact_table(raw_record, {"name", "size", "sha256"}, label)
        name = basename_value(record["name"], f"{label} name")
        if "\x00" in name or "\\" in name:
            fail(f"{label} name must be a safe filename")
        if name in declared:
            fail(f"device-data originals files must not contain duplicate names: {name}")
        size = integer_value(record["size"], f"{label} size", bounds=(1, 0xFFFFFFFF))
        digest = sha256_value(record["sha256"], f"{label} sha256")
        declared[name] = (size, digest)

    captured: dict[str, bytes] = {}
    for name, (size, digest) in declared.items():
        contents = _read_original(directory / name, f"device-data original {name}")
        if len(contents) != size or sha256_bytes(contents) != digest:
            fail(f"device-data original {name} does not match its manifest size and SHA-256")
        captured[name] = contents
    return OriginalFilesSource(manifest=raw, files=captured)

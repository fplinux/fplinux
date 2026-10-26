# SPDX-License-Identifier: GPL-2.0-only
"""Admit a target's complete named originals from one local manifest."""

from __future__ import annotations

import json
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically, sha256_bytes
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
    acquisition_recipe: str | None = None


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
    fields = {"target", "directory", "files"}
    if isinstance(value, dict) and "acquisition_recipe" in value:
        fields.add("acquisition_recipe")
    manifest = exact_table(value, fields, "device-data originals manifest")
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
    recipe = manifest.get("acquisition_recipe")
    if recipe is not None:
        recipe = sha256_value(recipe, "device-data originals acquisition_recipe")
    return OriginalFilesSource(manifest=raw, files=captured, acquisition_recipe=recipe)


def save_original_files(
    target: str,
    originals: dict[str, bytes],
    destination: Path,
    *,
    acquisition_recipe: str,
) -> Path:
    """Publish the complete selected source set under the caller's phone-store lock."""
    from fplinux_cli.device_data.prepare import (  # noqa: PLC0415 -- shared atomic directory publication.
        publish_phone_data,
    )
    from fplinux_cli.runtime.state import (  # noqa: PLC0415 -- private captured originals.
        private_directory,
    )

    sha256_value(acquisition_recipe, "originals acquisition_recipe")
    staging = Path(tempfile.mkdtemp(prefix=f".{destination.name}-", dir=destination.parent))
    try:
        private_directory(staging)
        records = []
        for name, contents in sorted(originals.items()):
            basename_value(name, "original filename")
            if "\\" in name or "\x00" in name or not contents:
                fail("original files require safe names and nonempty contents")
            replace_file_atomically(staging / name, contents, 0o600)
            records.append({"name": name, "size": len(contents), "sha256": sha256_bytes(contents)})
        manifest = {
            "target": target,
            "directory": ".",
            "files": records,
            "acquisition_recipe": acquisition_recipe,
        }
        replace_file_atomically(staging / "manifest.json", canonical_json_bytes(manifest), 0o600)
        read_original_files(target, staging / "manifest.json")
        publish_phone_data(staging, destination, replace=destination.exists())
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination / "manifest.json"

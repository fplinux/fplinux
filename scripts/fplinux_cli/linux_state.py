# SPDX-License-Identifier: GPL-2.0-only
"""Shared Linux source slots and their bounded upstream originals."""

from __future__ import annotations

import json
import shutil
import stat
import tarfile
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli import profile_layout
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.common import canonical_json_bytes, replace_file_atomically
from fplinux_cli.manifests.values import relative_value
from fplinux_cli.workspace import WorkspaceFile

if TYPE_CHECKING:
    from collections.abc import Iterable
    from pathlib import Path

MARKER_NAME = ".fplinux-recipe"
RECEIPT_NAME = ".fplinux-prepared.json"
BASE_NAME = ".fplinux-base"


class LinuxStateError(ValueError):
    """A prepared Linux tree cannot be used."""


@dataclass(frozen=True)
class PreparedLinuxState:
    """Selected image inputs and the shared tree they were prepared against."""

    linux_recipe: str
    tree_recipe: str


@dataclass(frozen=True)
class LinuxBase:
    """One extracted upstream source and its small collection of original files."""

    source: Path
    originals: Path


def _require_digest(value: object, field: str) -> str:
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise LinuxStateError(f"{field} must be a lowercase SHA-256 digest")
    return value


def _require_directory(path: Path, field: str) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        raise LinuxStateError(f"{field} is missing or invalid: {path}") from error
    if not stat.S_ISDIR(metadata.st_mode):
        raise LinuxStateError(f"{field} is missing or invalid: {path}")
    return path


def _ensure_real_directory(path: Path, field: str) -> Path:
    if not path.exists() and not path.is_symlink():
        path.mkdir()
    return _require_directory(path, field)


def ensure_sources_directory(cache: Path) -> Path:
    """Create and return the fixed shared-Linux cache directory."""
    if not cache.exists():
        cache.mkdir(parents=True)
    _require_directory(cache, "prepared Linux cache root")
    linux = _ensure_real_directory(cache / "linux", "prepared Linux cache directory")
    return _ensure_real_directory(linux / "sources", "prepared Linux cache directory")


def ensure_linux_base(
    cache: Path, source_lock: dict[str, Any], *, archive: Path | None = None
) -> LinuxBase:
    """Extract one complete tree per upstream digest, shared by every Linux consumer."""
    digest = _require_digest(source_lock.get("sha256"), "Linux source")
    version = source_lock.get("version")
    if not isinstance(version, str) or not version:
        message = "Linux source version must be a non-empty string"
        raise LinuxStateError(message)
    existing = inspect_linux_base(cache, digest)
    if existing is not None:
        return existing
    source = cache / "linux/sources" / digest
    originals = cache / "linux/originals" / digest

    parent = ensure_sources_directory(cache)
    linux = parent.parent
    originals_parent = _ensure_real_directory(linux / "originals", "Linux originals directory")
    staging_parent = _ensure_real_directory(linux / "staging", "Linux staging directory")
    staging = staging_parent / digest
    if staging.exists() or staging.is_symlink():
        _require_directory(staging, "Linux extraction staging")
        shutil.rmtree(staging)
    staging.mkdir()
    if archive is None:
        archive = sources_build.fetch(
            source_lock.get("url"), digest, cache / "downloads/linux", f"linux-{version}.tar.xz"
        )
    try:
        if source.exists() or source.is_symlink():
            _require_directory(source, "shared Linux source")
            shutil.rmtree(source)
        if originals.exists() or originals.is_symlink():
            _require_directory(originals, "Linux originals")
            shutil.rmtree(originals)
        with tarfile.open(archive, "r:*") as source_archive:
            source_archive.extractall(staging, filter="data")
        extracted = inputs_build.require_directory(staging / f"linux-{version}")
        inputs_build.require_file(extracted / "Makefile")
        _ensure_real_directory(originals_parent / digest, "Linux originals")
        replace_file_atomically(originals / "index.json", b"{}\n", 0o600, sync=False)
        (originals / BASE_NAME).write_text(f"{digest}\n")
        (extracted / BASE_NAME).write_text(f"{digest}\n")
        extracted.replace(source)
    finally:
        shutil.rmtree(staging)
    return LinuxBase(source, originals)


def inspect_linux_base(cache: Path, source_sha256: str) -> LinuxBase | None:
    """Find an already extracted source without touching its archive or writing files."""
    digest = _require_digest(source_sha256, "Linux source")
    source = cache / "linux/sources" / digest
    originals = cache / "linux/originals" / digest
    try:
        _require_directory(source, "shared Linux source")
        _require_directory(originals, "Linux originals")
        if (source / BASE_NAME).read_text() != f"{digest}\n":
            return None
        if (originals / BASE_NAME).read_text() != f"{digest}\n":
            return None
    except OSError, LinuxStateError:
        return None
    return LinuxBase(source, originals)


def original_paths(base: LinuxBase) -> dict[str, bool]:
    """Read the bounded set of captured paths, including originally absent files."""
    try:
        value = json.loads((base.originals / "index.json").read_text())
    except (OSError, ValueError) as error:
        message = "Linux original-file index is missing or invalid"
        raise LinuxStateError(message) from error
    if not isinstance(value, dict) or any(
        not isinstance(name, str) or not isinstance(present, bool)
        for name, present in value.items()
    ):
        message = "Linux original-file index is invalid"
        raise LinuxStateError(message)
    return value


def read_originals(base: LinuxBase, paths: Iterable[str]) -> dict[str, WorkspaceFile]:
    """Capture previously untouched paths, then return their upstream bytes and modes."""
    known = original_paths(base)
    requested = {relative_value(name, "Linux original path") for name in paths}
    missing = requested - known.keys()
    for name in sorted(missing):
        source = base.source / name
        present = source.exists() or source.is_symlink()
        if present:
            inputs_build.require_file(source)
            destination = base.originals / "files" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            destination.chmod(stat.S_IMODE(source.stat().st_mode))
        known[name] = present
    if missing:
        replace_file_atomically(
            base.originals / "index.json", canonical_json_bytes(known), 0o600, sync=False
        )
    result = {}
    for name in sorted(requested):
        if known[name]:
            path = inputs_build.require_file(base.originals / "files" / name)
            result[name] = WorkspaceFile(
                name, path.read_bytes(), stat.S_IMODE(path.stat().st_mode)
            )
    return result


def inspect_prepared_linux(
    source: Path, expected: PreparedLinuxState
) -> PreparedLinuxState | None:
    """Validate the aggregate receipt without scanning the shared source tree."""
    try:
        recipe = _require_digest(expected.tree_recipe, "prepared Linux tree recipe")
        _require_digest(expected.linux_recipe, "selected Linux recipe")
        source = _require_directory(source, "prepared Linux tree")
        marker = (source / MARKER_NAME).read_text()
        receipt = json.loads((source / RECEIPT_NAME).read_text())
    except LinuxStateError, OSError, ValueError:
        return None
    if marker != f"{recipe}\n" or receipt != {"tree_recipe": recipe}:
        return None
    return expected


def require_prepared_linux(source: Path, expected: PreparedLinuxState) -> PreparedLinuxState:
    """Reject a consumer whose shared preparation changed after selection."""
    if not isinstance(expected, PreparedLinuxState):
        message = "prepared Linux state is invalid"
        raise LinuxStateError(message)
    current = inspect_prepared_linux(source, expected)
    if current != expected:
        message = "prepared Linux tree changed after preparation"
        raise LinuxStateError(message)
    return current


def invalidate_prepared_linux(source: Path) -> None:
    """Remove successful preparation receipts before updating managed source files."""
    (source / MARKER_NAME).unlink(missing_ok=True)
    (source / RECEIPT_NAME).unlink(missing_ok=True)


def seal_prepared_linux(source: Path, state: PreparedLinuxState) -> PreparedLinuxState:
    """Publish the aggregate receipt only after every managed file is complete."""
    _require_directory(source, "prepared Linux source")
    recipe = _require_digest(state.tree_recipe, "prepared Linux tree recipe")
    replace_file_atomically(source / MARKER_NAME, f"{recipe}\n".encode(), 0o644, sync=False)
    replace_file_atomically(
        source / RECEIPT_NAME,
        canonical_json_bytes({"tree_recipe": recipe}),
        0o644,
        sync=False,
    )
    return state


def write_changed_file(path: Path, contents: bytes, mode: int) -> None:
    """Preserve the inode and mtime when a managed file already has the required value."""
    if path.exists():
        inputs_build.require_file(path)
        if path.read_bytes() == contents and stat.S_IMODE(path.stat().st_mode) == mode:
            return
    path.parent.mkdir(parents=True, exist_ok=True)
    replace_file_atomically(path, contents, mode, sync=False)


def write_profile_root(output: Path, target_config: dict[str, Any]) -> Path:
    """Keep profile-specific root boot arguments in this build's generated include path."""
    path = output / "include/generated/fplinux/fplinux-root.dtsi"
    contents = profile_layout.root_bootargs_dtsi(target_config["linux"]["root"])
    write_changed_file(path, contents, 0o644)
    return path

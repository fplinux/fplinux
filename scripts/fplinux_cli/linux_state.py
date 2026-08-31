# SPDX-License-Identifier: GPL-2.0-only
"""Exact recipe receipt for one prepared Linux source tree."""

from __future__ import annotations

import json
import shutil
import stat
from dataclasses import dataclass
from pathlib import Path

MARKER_NAME = ".fplinux-recipe"
RECEIPT_NAME = ".fplinux-prepared.json"


class LinuxStateError(ValueError):
    """A prepared Linux tree cannot be used."""


@dataclass(frozen=True)
class PreparedLinuxState:
    """The exact integration recipe of one prepared Linux tree."""

    linux_recipe: str

    def payload(self) -> dict[str, str]:
        """Return the receipt payload."""
        return {"linux_recipe": self.linux_recipe}


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
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
        raise LinuxStateError(f"{field} is missing or invalid: {path}")
    return path


def _ensure_real_directory(path: Path, field: str) -> Path:
    """Create one cache component only when it is absent, never through a symlink."""
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        try:
            path.mkdir()
        except OSError as error:
            raise LinuxStateError(f"{field} cannot be created: {path}") from error
    except OSError as error:
        raise LinuxStateError(f"{field} cannot be inspected: {path}") from error
    else:
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
            raise LinuxStateError(f"{field} is missing or invalid: {path}")
        return path
    return _require_directory(path, field)


def ensure_sources_directory(cache: Path) -> Path:
    """Create and return the fixed prepared-Linux cache directory."""
    cache = Path(cache)
    try:
        cache.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        message = f"prepared Linux cache directory cannot be created: {cache}"
        raise LinuxStateError(message) from error
    _require_directory(cache, "prepared Linux cache root")
    linux = _ensure_real_directory(cache / "linux", "prepared Linux cache directory")
    return _ensure_real_directory(linux / "sources", "prepared Linux cache directory")


def inspect_prepared_linux(source: Path, expected_recipe: str) -> PreparedLinuxState | None:
    """Return a hit only for the exact prepared-tree recipe."""
    try:
        recipe = _require_digest(expected_recipe, "prepared Linux recipe")
        source = _require_directory(Path(source), "prepared Linux tree")
        marker = (source / MARKER_NAME).read_text(encoding="utf-8")
        receipt = json.loads((source / RECEIPT_NAME).read_text(encoding="utf-8"))
    except LinuxStateError, OSError, json.JSONDecodeError:
        return None
    state = PreparedLinuxState(recipe)
    if marker != f"{recipe}\n" or receipt != state.payload():
        return None
    return state


def require_prepared_linux(source: Path, expected: PreparedLinuxState) -> PreparedLinuxState:
    """Require that a prepared tree still has the expected recipe receipt."""
    if not isinstance(expected, PreparedLinuxState):
        message = "prepared Linux state is invalid"
        raise LinuxStateError(message)
    current = inspect_prepared_linux(source, expected.linux_recipe)
    if current != expected:
        message = "prepared Linux tree changed after preparation"
        raise LinuxStateError(message)
    return current


def seal_prepared_linux(source: Path, linux_recipe: str) -> PreparedLinuxState:
    """Write the exact recipe marker and receipt into a completed staging tree."""
    source = _require_directory(Path(source), "prepared Linux staging tree")
    state = PreparedLinuxState(_require_digest(linux_recipe, "prepared Linux recipe"))
    payload = json.dumps(state.payload(), sort_keys=True, separators=(",", ":")) + "\n"
    try:
        (source / MARKER_NAME).write_text(f"{state.linux_recipe}\n", encoding="utf-8")
        (source / RECEIPT_NAME).write_text(payload, encoding="utf-8")
    except OSError as error:
        raise LinuxStateError(f"prepared Linux receipt cannot be written: {source}") from error
    return state


def publish_prepared_linux(source: Path, staging: Path) -> None:
    """Replace the old prepared tree with the completed staging tree."""
    source = Path(source)
    staging = _require_directory(Path(staging), "prepared Linux staging tree")
    _require_directory(source.parent, "prepared Linux destination parent")
    try:
        if source.is_symlink():
            raise LinuxStateError(f"prepared Linux destination is missing or invalid: {source}")
        if source.exists():
            _require_directory(source, "prepared Linux destination")
            shutil.rmtree(source)
        _require_directory(source.parent, "prepared Linux destination parent")
        staging.replace(source)
    except OSError as error:
        raise LinuxStateError(f"prepared Linux tree could not be published: {source}") from error

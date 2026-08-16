# SPDX-License-Identifier: GPL-2.0-only
"""Cache successful exact per-scope checks."""

from __future__ import annotations

import hashlib
import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, TypeVar

if TYPE_CHECKING:
    from collections.abc import Callable

_SCOPE_ROOT = "check-results"
_T = TypeVar("_T")


def _canonical_json(value: object) -> bytes:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return encoded.encode() + b"\n"


@dataclass(frozen=True)
class CheckReceiptRecipe:
    """Every input that permits a successful check result to be reused."""

    scope: str
    closure_digest: str
    orchestration_recipe: str
    image_identity: str
    commands: tuple[tuple[str, ...], ...]

    def payload(self) -> dict[str, object]:
        """Return the exact persisted payload for one successful check."""
        return {
            "result": "success",
            "scope": self.scope,
            "closure_digest": self.closure_digest,
            "orchestration_recipe": self.orchestration_recipe,
            "image_identity": self.image_identity,
            "commands": [list(command) for command in self.commands],
        }


def check_closure_digest(files: list[tuple[str, Path]]) -> str:
    """Hash the checked files' paths, bytes, and modes."""
    return check_closure_entries_digest(
        [(relative, path.read_bytes(), path.stat().st_mode & 0o777) for relative, path in files],
    )


def check_closure_entries_digest(entries: list[tuple[str, bytes, int]]) -> str:
    """Hash one captured check closure."""
    value = hashlib.sha256()
    value.update(b"fplinux.check-closure\0")
    for relative, contents, mode in sorted(entries):
        for field in (relative.encode(), mode.to_bytes(2, "big"), contents):
            value.update(len(field).to_bytes(8, "big"))
            value.update(field)
    return value.hexdigest()


def receipt_path(cache_root: Path, recipe: CheckReceiptRecipe) -> Path:
    """Return the one receipt path for a scope."""
    return cache_root / _SCOPE_ROOT / recipe.scope / "success.json"


def receipt_matches(cache_root: Path, recipe: CheckReceiptRecipe) -> bool:
    """Return whether an exact successful receipt exists."""
    return _read_payload(receipt_path(cache_root, recipe)) == recipe.payload()


def _read_payload(path: Path) -> object | None:
    """Read a receipt; unreadable or invalid JSON is a cache miss."""
    try:
        with path.open(encoding="utf-8") as stream:
            value: object = json.load(stream)
            return value
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None


def publish_success_receipt(cache_root: Path, recipe: CheckReceiptRecipe) -> Path:
    """Atomically publish a receipt after a scope has completed successfully."""
    destination = receipt_path(cache_root, recipe)
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=destination.parent,
        prefix=f".{destination.stem}.",
        suffix=".tmp",
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(_canonical_json(recipe.payload()))
        Path(temporary_name).replace(destination)
    finally:
        Path(temporary_name).unlink(missing_ok=True)
    return destination


def run_and_publish_success(
    cache_root: Path,
    recipe: CheckReceiptRecipe,
    operation: Callable[[], _T],
) -> _T:
    """Run one scope and cache its receipt only if it succeeds."""
    result = operation()
    publish_success_receipt(cache_root, recipe)
    return result

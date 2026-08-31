# SPDX-License-Identifier: GPL-2.0-only
"""Cache successful exact per-scope checks."""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass
from typing import TYPE_CHECKING

from .config import TARGET_NAME

if TYPE_CHECKING:
    from pathlib import Path

_SCOPE_ROOT = "check-results"


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
    profile: str | None = None

    def payload(self) -> dict[str, object]:
        """Return the exact persisted payload for one successful check."""
        return {
            "result": "success",
            "scope": self.scope,
            "closure_digest": self.closure_digest,
            "orchestration_recipe": self.orchestration_recipe,
            "image_identity": self.image_identity,
            "commands": [list(command) for command in self.commands],
            "profile": self.profile,
        }


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
    """Return the one default or named-profile receipt path for a scope."""
    if recipe.profile is None:
        return cache_root / _SCOPE_ROOT / recipe.scope / "success.json"
    if TARGET_NAME.fullmatch(recipe.profile) is None:
        raise ValueError(f"invalid receipt profile: {recipe.profile!r}")
    return cache_root / _SCOPE_ROOT / "profiles" / recipe.profile / recipe.scope / "success.json"


def receipt_matches(cache_root: Path, recipe: CheckReceiptRecipe) -> bool:
    """Return whether an exact successful receipt exists."""
    return _read_payload(receipt_path(cache_root, recipe)) == recipe.payload()


def _read_payload(path: Path) -> object | None:
    """Read a receipt; unreadable or invalid JSON is a cache miss."""
    if path.is_symlink():
        return None
    try:
        with path.open(encoding="utf-8") as stream:
            value: object = json.load(stream)
            return value
    except OSError, UnicodeDecodeError, json.JSONDecodeError:
        return None


def publish_success_receipt(cache_root: Path, recipe: CheckReceiptRecipe) -> None:
    """Atomically publish a receipt after a scope has completed successfully."""
    destination = receipt_path(cache_root, recipe)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.parent / f".{destination.stem}.tmp"
    descriptor = os.open(
        temporary,
        os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW,
        0o600,
    )
    with os.fdopen(descriptor, "wb") as stream:
        try:
            stream.write(_canonical_json(recipe.payload()))
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    temporary.replace(destination)

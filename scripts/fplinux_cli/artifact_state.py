# SPDX-License-Identifier: GPL-2.0-only
"""Small shared primitives for exact profile-owned artifact receipts."""

from __future__ import annotations

import json
from collections.abc import Callable
from typing import TYPE_CHECKING

from .common import sha256_file

if TYPE_CHECKING:
    from pathlib import Path

Failure = Callable[[str], Exception]


def canonical_json_bytes(value: object) -> bytes:
    """Encode one deterministic JSON receipt or causal manifest."""
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    ).encode()


def require_lowercase_sha256(value: object, field: str, failure: Failure) -> str:
    """Return one canonical SHA-256 digest or raise the producer's error."""
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        raise failure(f"{field} must be a lowercase SHA-256 digest")
    return value


def regular_file_record(path: Path, description: str, failure: Failure) -> dict[str, int | str]:
    """Describe one real regular input or published artifact file."""
    if path.is_symlink() or not path.is_file():
        raise failure(f"{description} is missing or invalid: {path}")
    metadata = path.stat()
    return {
        "mode": metadata.st_mode & 0o777,
        "sha256": sha256_file(path),
        "size": metadata.st_size,
    }


def read_json_object(path: Path) -> dict[str, object] | None:
    """Read a receipt only when it is an ordinary JSON object."""
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def write_canonical_json(path: Path, value: object, *, mode: int | None = None) -> None:
    """Write one deterministic receipt, optionally with its required mode."""
    path.write_bytes(canonical_json_bytes(value))
    if mode is not None:
        path.chmod(mode)


def receipt_matches(path: Path, expected: dict[str, object]) -> bool:
    """Compare one complete independently recomputed receipt payload exactly."""
    return read_json_object(path) == expected

# SPDX-License-Identifier: GPL-2.0-only
"""Small shared primitives for exact profile-owned artifact receipts."""

from __future__ import annotations

from collections.abc import Callable
from typing import TYPE_CHECKING

from .common import canonical_json_bytes, read_json_object, sha256_file

if TYPE_CHECKING:
    from pathlib import Path

Failure = Callable[[str], Exception]


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


def write_canonical_json(path: Path, value: object, *, mode: int | None = None) -> None:
    """Write one deterministic receipt, optionally with its required mode."""
    path.write_bytes(canonical_json_bytes(value))
    if mode is not None:
        path.chmod(mode)


def receipt_matches(path: Path, expected: dict[str, object]) -> bool:
    """Compare one complete independently recomputed receipt payload exactly."""
    return read_json_object(path) == expected

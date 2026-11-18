# SPDX-License-Identifier: GPL-2.0-only
"""Read the prepared host-tool binary and its content receipt."""

from __future__ import annotations

import stat
from typing import TYPE_CHECKING

from fplinux_cli.common import is_sha256, read_json_object, sha256_file

if TYPE_CHECKING:
    from pathlib import Path


def read_cached_host_tool(slot: Path, *, recipe: str | None = None) -> Path | None:
    """Return the admitted binary, optionally requiring the selected build recipe."""
    receipt = slot / "receipt.json"
    binary = slot / "binary"
    if receipt.is_symlink() or binary.is_symlink() or not binary.is_file():
        return None
    record = read_json_object(receipt)
    if (
        record is None
        or not is_sha256(record.get("recipe"))
        or not is_sha256(record.get("sha256"))
        or (recipe is not None and record["recipe"] != recipe)
    ):
        return None
    try:
        if stat.S_IMODE(binary.stat().st_mode) != 0o755 or sha256_file(binary) != record["sha256"]:
            return None
    except OSError:
        return None
    return binary

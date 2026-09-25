# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""UMS9117 template and placeholder values for ./fplinux target new."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Mapping


def template_directory() -> Path:
    """Return the tree whose files, with placeholders filled, form a new target."""
    return Path(__file__).resolve().parent.parent / "target-template"


def template_values(target: str, identity: Mapping[str, Any]) -> dict[str, str]:
    """Return every @NAME@ value used by the template for one validated target identity."""
    return {
        "TARGET": target,
        # The bootstrap record prefix and the bootstrap image and map file stem.
        "SYMBOL": target.upper().replace("-", "_"),
        "FILE_STEM": target.replace("-", "_"),
        "BRAND": identity["brand"],
        "PRODUCT": identity["product"],
        "COMPATIBLE": identity["compatible"],
        "DEVICE": identity["display_name"],
    }

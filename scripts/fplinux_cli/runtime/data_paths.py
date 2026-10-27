# SPDX-License-Identifier: GPL-2.0-only
"""Keep mutable runtime state separate for each native installation path."""

from __future__ import annotations

import hashlib
import os
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def installation_id(anchor: Path) -> str:
    """Identify a canonical native installation independently of its bundle contents."""
    canonical = os.path.normcase(str(anchor.resolve()))
    return hashlib.sha256(os.fsencode(canonical)).hexdigest()


def checkout_runtime_root(managed_root: Path, anchor: Path, target: str) -> Path:
    """Keep checkout sessions apart even when installations share a data root."""
    return managed_root / "runtime/checkouts" / installation_id(anchor) / target


def archive_runtime_root(managed_root: Path, anchor: Path) -> Path:
    """Keep each extracted archive's sessions and logs in its own writable scope."""
    return managed_root / "runtime/archives" / installation_id(anchor)

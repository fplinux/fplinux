# SPDX-License-Identifier: GPL-2.0-only
"""Resolve the selected installation's managed data directory without creating it."""

from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Mapping

DATA_ENV = "FPLINUX_DATA_DIR"
DATA_DIR_NAME = ".data"
PRIVATE_DIR_NAME = ".private"


def data_root(anchor: Path, *, environment: Mapping[str, str] | None = None) -> Path:
    """Resolve native absolute or installation-relative paths without shell expansion."""
    if environment is None:
        environment = os.environ
    value = environment.get(DATA_ENV)
    if value is None:
        selected = anchor / DATA_DIR_NAME
    else:
        if not value or any(character in value for character in ("\0", "\n", "\r")):
            message = f"{DATA_ENV} must be a non-empty path without NUL or newlines"
            raise ValueError(message)
        selected = Path(value)
        if os.name == "nt" and (selected.drive or selected.root) and not selected.is_absolute():
            message = f"{DATA_ENV} must be fully qualified or relative to the installation"
            raise ValueError(message)
        if not selected.is_absolute():
            selected = anchor / selected
    return selected.resolve()

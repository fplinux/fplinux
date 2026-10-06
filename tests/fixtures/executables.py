# SPDX-License-Identifier: GPL-2.0-only
"""Install static Python fixture programs with the current test interpreter."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


def install_python_script(source: Path, destination: Path, *, mode: int = 0o755) -> None:
    """Keep executable fixtures on the interpreter and permissions chosen by their test."""
    destination.write_text(f"#!{sys.executable}\n" + source.read_text(), encoding="utf-8")
    destination.chmod(mode)

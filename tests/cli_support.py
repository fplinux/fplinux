# SPDX-License-Identifier: GPL-2.0-only
"""Minimal isolated checkout for public commands that do not need build inputs."""

from __future__ import annotations

import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def prepare_cli_checkout(root: Path) -> None:
    """Copy the real CLI with one discoverable target, without runtime or cache state."""
    shutil.copy(ROOT / "fplinux", root / "fplinux")
    shutil.copytree(ROOT / "scripts/fplinux_cli", root / "scripts/fplinux_cli")
    target = root / "targets/example"
    target.mkdir(parents=True)
    (target / "target.toml").touch()

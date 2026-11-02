# SPDX-License-Identifier: GPL-2.0-only
"""Run Windows driver recipe help from a relocated source file."""

from __future__ import annotations

import shutil
import subprocess
import sys
from typing import TYPE_CHECKING

from fplinux_cli.build.host_tools import windows_driver

if TYPE_CHECKING:
    from pathlib import Path


def test_relocated_driver_build_recipe_runs_without_the_project_package(tmp_path: Path) -> None:
    """Run only argument parsing, without compilation or Windows execution."""
    recipe = tmp_path / "build.py"
    shutil.copyfile(windows_driver.__file__, recipe)
    result = subprocess.run(
        [sys.executable, "-I", str(recipe), "--help"],
        cwd=tmp_path,
        capture_output=True,
        text=True,
        check=True,
    )
    assert "--toolchain" in result.stdout
    assert "--source-date-epoch" in result.stdout

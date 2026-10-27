# SPDX-License-Identifier: GPL-2.0-only
"""Canary whose expected native failure must remain visible outside pytest."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT

if TYPE_CHECKING:
    from pathlib import Path


def test_expected_child_failure(tmp_path: Path) -> None:
    """Pytest succeeds even though its native child reports fatal undefined behavior."""
    executable = tmp_path / "expected-child"
    subprocess.run(
        ["cc", "-O0", str(ROOT / "tests/sanitizer/undefined.c"), "-o", str(executable)],
        check=True,
        timeout=30,
    )
    result = subprocess.run(
        [str(executable)], env={}, capture_output=True, text=True, timeout=30, check=False
    )
    assert result.returncode != 0

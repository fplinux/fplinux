# SPDX-License-Identifier: GPL-2.0-only
"""Protect native smoke imports independently of Linux build dependencies."""

import sys
from pathlib import Path

from tests.process import python_environment, run_process


def test_native_smoke_discovery_does_not_require_unix_only_modules() -> None:
    result = run_process(
        [sys.executable, str(Path(__file__).parent / "fixtures/native-import.py")],
        name="native smoke import boundary",
        env=python_environment(),
        timeout=10,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout == "native smoke discovery without Unix modules: OK\n"

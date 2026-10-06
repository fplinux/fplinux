# SPDX-License-Identifier: GPL-2.0-only
"""Public test-command argument handling without starting a runtime."""

from __future__ import annotations

from pathlib import Path

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class TestCommandCliTests:
    """Help and malformed selections complete before workspace or runtime setup."""

    def test_help_explains_selection_and_execution_options(self) -> None:
        """Expose exact selection, tier discovery and runner controls."""
        result = run_process(
            [str(ROOT / "fplinux"), "test", "--help"], name="test help", timeout=10, cwd=ROOT
        )
        assert (result.returncode) == (0), result.stderr
        for option in ("--tier", "--verbose", "--failfast"):
            assert (option) in (result.stdout)
        assert ("module, class or method") in (result.stdout)
        assert (result.stderr) == ("")

    @pytest.mark.parametrize(
        "arguments",
        [
            ("--tier", "unknown"),
            ("tests.small.environment.test_common", "--tier", "small"),
            (".cache.private_test",),
            ("tests.small.environment.test_common..method",),
            ("tests/small/../../private_test.py",),
            ("/outside/test_private.py",),
            ("tests/small/test_selection.py::--help",),
            ("--collect-only",),
        ],
    )
    def test_invalid_or_conflicting_selection_is_a_usage_error(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Reject invalid CLI syntax instead of silently running a broader suite."""
        result = run_process(
            [str(ROOT / "fplinux"), "test", *arguments],
            name="invalid test selection",
            timeout=10,
            cwd=ROOT,
        )
        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("workspace") not in (result.stderr)
        assert ("Traceback") not in (result.stderr)

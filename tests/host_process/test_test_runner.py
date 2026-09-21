# SPDX-License-Identifier: GPL-2.0-only
"""Exercise selection and result propagation through real unittest subprocesses."""

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from tests.process import python_environment, run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]


class TestSelectionProcessTests(unittest.TestCase):
    """Run the in-image entry point on controlled files; this is not a Kern test."""

    def setUp(self) -> None:
        """Keep the fixture package and its outcomes independent of the real suite."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.trace = self.root / "executed.txt"
        (self.root / "tests").mkdir()
        (self.root / "tests/__init__.py").touch()
        for tier in ("small", "host_process", "host_tool", "artifact", "public_workflow"):
            directory = self.root / "tests" / tier
            directory.mkdir()
            (directory / "__init__.py").touch()
            shutil.copyfile(
                ROOT / "tests/fixtures/processes/selection_cases.py",
                directory / "test_selection.py",
            )

    def run_selection(
        self, *arguments: str, failing_fixture: bool = True
    ) -> subprocess.CompletedProcess[str]:
        """Execute the production entry point with only process-environment boundaries supplied."""
        self.logs = Path(tempfile.mkdtemp(dir=self.root, prefix="logs-"))
        return run_process(
            [sys.executable, "-m", "fplinux_cli.quality.testing", *arguments],
            name="selected unittest process",
            cwd=self.root,
            env={
                **python_environment(),
                "FPLINUX_TEST_TRACE": str(self.trace),
                "FPLINUX_TEST_FAIL": "1" if failing_fixture else "0",
                "FPLINUX_LOG_ROOT": str(self.logs),
                "FPLINUX_VERBOSE": "1" if "--verbose" in arguments else "0",
            },
            timeout=15,
        )

    def executed(self) -> list[str]:
        """Read the independent fixture's observed execution trace."""
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def test_class_and_method_select_only_requested_cases(self) -> None:
        """A class runs its two tests; a method does not run its sibling."""
        prefix = "tests.small.test_selection.PassingCase"
        result = self.run_selection(prefix)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.executed(), [prefix + ".test_first", prefix + ".test_second"])
        self.trace.unlink()
        result = self.run_selection(prefix + ".test_second", "--verbose")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.executed(), [prefix + ".test_second"])
        self.assertIn("test_second", (self.logs / "tests/01-selected.log").read_text())
        self.assertIn("test_second", result.stderr)

    def test_no_selection_runs_every_tier_in_order(self) -> None:
        """Default discovery must execute all five groups, not just the first group."""
        result = self.run_selection(failing_fixture=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.executed(),
            [
                f"tests.{tier}.test_selection.{case}.{method}"
                for tier in ("small", "host_process", "host_tool", "artifact", "public_workflow")
                for case in ("FailureCase", "PassingCase")
                for method in ("test_first", "test_second")
            ],
        )

    def test_multiple_names_preserve_each_selection(self) -> None:
        """Names from different tiers run together without discovering neighboring tests."""
        names = (
            "tests.small.test_selection.PassingCase.test_second",
            "tests.host_tool.test_selection.PassingCase.test_first",
        )
        result = self.run_selection(*names)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.executed(), list(names))

    def test_module_and_tier_discovery_propagate_failures(self) -> None:
        """Both selection forms run the chosen module and not another tier."""
        prefix = "tests.small.test_selection"
        expected = [
            prefix + ".FailureCase.test_first",
            prefix + ".FailureCase.test_second",
            prefix + ".PassingCase.test_first",
            prefix + ".PassingCase.test_second",
        ]
        for arguments in ((prefix,), ("--tier", "small")):
            with self.subTest(arguments=arguments):
                if self.trace.exists():
                    self.trace.unlink()
                result = self.run_selection(*arguments)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(self.executed(), expected)
                self.assertIn("intentional selection-fixture failure", result.stderr)

    def test_failfast_stops_at_the_first_failure(self) -> None:
        """Failfast is a unittest behavior, not just an accepted unused flag."""
        prefix = "tests.small.test_selection.FailureCase"
        result = self.run_selection(prefix, "--failfast")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.executed(), [prefix + ".test_first"])

    def test_missing_or_empty_selection_is_not_success(self) -> None:
        """Unknown names fail and a valid empty class retains unittest's no-tests status."""
        for suffix, status in (("MissingCase", 1), ("EmptyCase", 5)):
            with self.subTest(suffix=suffix):
                result = self.run_selection("tests.small.test_selection." + suffix)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(self.executed(), [])


if __name__ == "__main__":
    unittest.main()

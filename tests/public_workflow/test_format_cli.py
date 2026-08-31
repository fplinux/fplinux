# SPDX-License-Identifier: GPL-2.0-only
"""Public command-line evidence for explicit source formatting."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]


class FormatCliWorkflowTests(unittest.TestCase):
    """Exercise public parser and pre-runtime refusal behavior."""

    def run_format(self, *arguments: str, root: Path = ROOT) -> subprocess.CompletedProcess[str]:
        """Invoke the repository entrypoint with a bounded process lifetime."""
        return run_process(
            [str(root / "fplinux"), "format", *arguments],
            name="public fplinux format",
            timeout=30,
            cwd=root,
        )

    def test_help_requires_explicit_repository_relative_paths(self) -> None:
        """Advertise a path-only mutating interface without recursive defaults."""
        result = self.run_format("--help")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PATH [PATH ...]", result.stdout)
        self.assertNotIn("--all", result.stdout)

    def test_missing_path_is_rejected_by_the_public_parser(self) -> None:
        """Formatting cannot silently expand to the complete checkout."""
        result = self.run_format()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("the following arguments are required: PATH", result.stderr)

    def test_unsupported_source_is_rejected_before_runtime(self) -> None:
        """A checker-only source gets a named public error without tool execution."""
        with tempfile.TemporaryDirectory() as temporary:
            checkout = Path(temporary) / "source"
            shutil.copytree(
                ROOT,
                checkout,
                ignore=shutil.ignore_patterns(".git", ".cache", "__pycache__"),
            )
            for command in (("git", "init", "-q"), ("git", "add", "--all")):
                prepared = run_process(
                    command,
                    name="temporary Git source inventory",
                    timeout=30,
                    cwd=checkout,
                )
                self.assertEqual(prepared.returncode, 0, prepared.stderr)
            result = self.run_format(
                "targets/nokia-ta1618/release/README.txt",
                root=checkout,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no project formatter is defined", result.stderr)
        self.assertNotIn("kern", result.stdout.lower())
        self.assertNotIn("kern", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()

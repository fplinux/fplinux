# SPDX-License-Identifier: GPL-2.0-only
"""Public command-line evidence for explicit source formatting."""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]


class FormatCliWorkflowTests:
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

        assert (result.returncode) == (0), result.stderr
        assert ("PATH [PATH ...]") in (result.stdout)
        assert ("--all") not in (result.stdout)

    def test_missing_path_is_rejected_by_the_public_parser(self) -> None:
        """Formatting cannot silently expand to the complete checkout."""
        result = self.run_format()

        assert (result.returncode) != (0)
        assert ("the following arguments are required: PATH") in (result.stderr)

    def test_unsupported_source_is_rejected_before_runtime(self, tmp_path: Path) -> None:
        """A checker-only source gets a named public error without tool execution."""
        checkout = tmp_path / "source"
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
            assert (prepared.returncode) == (0), prepared.stderr
        result = self.run_format(
            "targets/nokia-ta1618/release/README.txt",
            root=checkout,
        )

        assert (result.returncode) != (0)
        assert ("no project formatter is defined") in (result.stderr)
        assert ("kern") not in (result.stdout.lower())
        assert ("kern") not in (result.stderr.lower())

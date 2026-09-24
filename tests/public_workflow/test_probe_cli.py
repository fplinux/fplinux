# SPDX-License-Identifier: GPL-2.0-only
"""Public probe parser and path validation without a compiler or phone."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from tests.cli_support import ROOT, prepare_cli_checkout
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess


class ProbeCliTests(unittest.TestCase):
    """Exercise the real entrypoint in an isolated checkout with no Kern state."""

    def setUp(self) -> None:
        """Provide one source and no ambient build image or cache."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        prepare_cli_checkout(self.root)
        (self.root / "probe.c").write_text("int main(void) { return 0; }\n")

    def run_probe(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Invoke the public command with bounded lifetime."""
        return run_process(
            [str(self.root / "fplinux"), "probe-build", *arguments],
            name="public probe-build validation",
            cwd=self.root,
            timeout=10,
        )

    def test_parser_requires_one_source_and_explicit_output(self) -> None:
        """The command cannot infer output or accept arbitrary compiler flags."""
        help_result = self.run_probe("--help")
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("SOURCE.c", help_result.stdout)
        self.assertIn("--output .cache/tools/NAME", help_result.stdout)
        for arguments in (
            (),
            ("probe.c",),
            ("probe.c", "probe.c", "--output", ".cache/tools/probe"),
            ("probe.c", "--output", ".cache/tools/probe", "-static"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_probe(*arguments)
                self.assertEqual(result.returncode, 2, result.stderr)

    def test_invalid_paths_are_rejected_before_environment_preparation(self) -> None:
        """Reject outside, unnormalized, missing and non-C inputs without building."""
        cases = (
            ("../probe.c", ".cache/tools/probe", "normalized relative path"),
            ("./probe.c", ".cache/tools/probe", "normalized relative path"),
            (str(self.root / "probe.c"), ".cache/tools/probe", "normalized relative path"),
            ("missing.c", ".cache/tools/probe", "regular .c file"),
            ("fplinux", ".cache/tools/probe", "regular .c file"),
            ("probe.c", "probe", "inside .cache/tools"),
            ("probe.c", ".cache/tools", "inside .cache/tools"),
            ("probe.c", ".cache/tools/../probe", "normalized relative path"),
        )
        for source, output, message in cases:
            with self.subTest(source=source, output=output):
                result = self.run_probe(source, "--output", output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertFalse((self.root / output).is_file())

    def test_symlinks_and_nonregular_outputs_are_refused(self) -> None:
        """Neither a source link nor a linked output parent can redirect the command."""
        tools = self.root / ".cache/tools"
        tools.mkdir(parents=True)
        (self.root / "link.c").symlink_to("probe.c")
        (tools / "link").symlink_to(self.root, target_is_directory=True)
        (tools / "directory").mkdir()
        for source, output, message in (
            ("link.c", ".cache/tools/probe", "must not use a symlink"),
            ("probe.c", ".cache/tools/link/probe", "must not use a symlink"),
            ("probe.c", ".cache/tools/directory", "regular file"),
        ):
            with self.subTest(source=source, output=output):
                result = self.run_probe(source, "--output", output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stderr)

    def test_ignored_source_reaches_the_explicit_setup_requirement(self) -> None:
        """An ignored local probe is accepted, but missing Kern never triggers setup."""
        for relative in (
            "Containerfile",
            "container.lock.toml",
            ".kernignore",
            "package.json",
            "package-lock.json",
        ):
            shutil.copy(ROOT / relative, self.root / relative)
        tools = self.root / ".cache/tools"
        tools.mkdir(parents=True)
        source = tools / "local.c"
        source.write_text("int main(void) { return 0; }\n")
        result = self.run_probe(".cache/tools/local.c", "--output", ".cache/tools/local")

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("./fplinux setup", result.stderr)
        self.assertFalse((tools / "local").exists())
        self.assertFalse((self.root / ".cache/kern").exists())


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Alpine packing process environment, working directory and stage-log routing."""

from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.alpine import rootfs_files as alpine_builder
from fplinux_cli.reporting.run import RunReporter


class AlpinePackingProcessTests(unittest.TestCase):
    """A stub cpio child observes execution context; it does not create an archive."""

    def test_packing_child_receives_build_environment_and_logs_errors(self) -> None:
        """Stage routing preserves child context and rejects a nonzero packing result."""
        for stage_enabled, status in ((False, 0), (False, 7), (True, 0), (True, 7)):
            with (
                self.subTest(stage_enabled=stage_enabled, status=status),
                tempfile.TemporaryDirectory() as directory,
            ):
                temporary = Path(directory)
                root = temporary / "root"
                root.mkdir()
                (root / "payload").write_text("example\n")
                tools = temporary / "tools"
                tools.mkdir()
                cpio = tools / "cpio"
                cpio.write_text(
                    f"#!{sys.executable}\n"
                    "import json, os, sys\n"
                    "sys.stdin.buffer.read()\n"
                    "print(json.dumps({'cwd': os.getcwd(), 'locale': os.environ['LC_ALL'], "
                    "'user': os.environ['KBUILD_BUILD_USER']}))\n"
                    "print('packing diagnostic', file=sys.stderr)\n"
                    f"sys.exit({status})\n"
                )
                cpio.chmod(0o755)
                destination = temporary / "rootfs.cpio"
                reporter = RunReporter("packing", temporary / "logs", "logs", verbose=False)
                stderr = io.StringIO()
                stdout = io.StringIO()
                stage = reporter.stage("pack") if stage_enabled else contextlib.nullcontext()
                with (
                    mock.patch.dict(os.environ, {"PATH": f"{tools}:{os.environ['PATH']}"}),
                    contextlib.redirect_stderr(stderr),
                    contextlib.redirect_stdout(stdout),
                ):
                    if status:
                        if stage_enabled:
                            with self.assertRaises(SystemExit) as stage_error, stage:
                                alpine_builder._write_rootfs_cpio(root, destination)  # noqa: SLF001
                            self.assertEqual(stage_error.exception.code, 7)
                        else:
                            with self.assertRaises(subprocess.CalledProcessError) as process_error:
                                alpine_builder._write_rootfs_cpio(root, destination)  # noqa: SLF001
                            self.assertEqual(process_error.exception.returncode, 7)
                    else:
                        with stage:
                            alpine_builder._write_rootfs_cpio(root, destination)  # noqa: SLF001
                reporter.finish()
                self.assertEqual(
                    json.loads(destination.read_text()),
                    {"cwd": str(root), "locale": "C", "user": "fplinux"},
                )
                if stage_enabled:
                    log = (temporary / "logs/01-pack.log").read_text()
                    self.assertIn("packing diagnostic", log)
                    self.assertEqual(stdout.getvalue(), "")


if __name__ == "__main__":
    unittest.main()

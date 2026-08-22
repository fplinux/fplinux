# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for kernel-check status handling and analyzer work isolation.

The work-isolation test replaces every external analyzer with a no-op stub. It verifies
cleanup behavior; it does not run Clang, checkpatch, Kbuild, dtbs_check, Sparse, a
cross-compiler, or a kernel build.
"""

from __future__ import annotations

import contextlib
import io
import signal
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import kernelcheck
from fplinux_cli.linux_state import PreparedLinuxState


class KernelCheckTests(unittest.TestCase):
    """Preserve shell-visible statuses from captured policy commands."""

    def test_checkpatch_signal_uses_shell_status(self) -> None:
        """Convert a negative subprocess return code before raising."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
        ]
        with contextlib.redirect_stdout(terminal), self.assertRaises(SystemExit) as raised:
            kernelcheck.run_checkpatch(command)
        self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)
        self.assertIn(f"checkpatch exited {-signal.SIGTERM}\n", terminal.getvalue())

    def test_dtbs_signal_uses_shell_status(self) -> None:
        """Convert a signalled dtbs_check status before propagating it."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGKILL)",
        ]
        with contextlib.redirect_stdout(terminal), self.assertRaises(SystemExit) as raised:
            kernelcheck.run_dtbs_check(command, "test-target")
        self.assertEqual(raised.exception.code, 128 + signal.SIGKILL)
        self.assertIn(
            f"dtbs_check exited {-signal.SIGKILL}: test-target\n",
            terminal.getvalue(),
        )


class KernelAnalyzerWorkIsolationTests(unittest.TestCase):
    """Check disposable analyzer work while replacing external commands with stubs."""

    target = "test-target"

    def setUp(self) -> None:
        """Create a small projected Linux context without invoking kernel tools."""
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.cache = self.root / ".cache"
        self.source = self.root / "linux"
        (self.source / "scripts").mkdir(parents=True)
        (self.source / ".clang-format").write_text("BasedOnStyle: LLVM\n")
        (self.source / "scripts/checkpatch.pl").write_text("#!/usr/bin/env perl\n")
        self.defconfig = self.root / "defconfig"
        self.defconfig.write_text("CONFIG_TEST=y\n")
        self.projected = self.root / "driver.c"
        self.projected.write_text("int test_driver;\n")
        self.prepared_linux = PreparedLinuxState("a" * 64)
        self.target_config = {
            "linux": {
                "defconfig": "defconfig",
                "patches": [],
                "copies": [],
                "appends": [],
            }
        }
        self.platform = {
            "linux": {
                "arch": "arm",
                "analysis_cross_compile": "arm-linux-gnueabihf-",
                "patches": [],
            }
        }

    def tearDown(self) -> None:
        """Remove the isolated fake cache and prepared source tree."""
        self.temporary.cleanup()

    def _run_check(self) -> None:
        """Run the check flow with command stand-ins and a fixed temporary cache."""
        output = self._cache_path("work")
        state = self.prepared_linux

        def run_command(command: list[str]) -> None:
            if command[-1:] == ["savedefconfig"]:
                (output / "defconfig").write_text(self.defconfig.read_text())

        def run_dtbs_check(_command: list[str], _target: str) -> str:
            return ""

        def target_source(_target: str, _relative: str) -> Path:
            return self.defconfig

        with (
            mock.patch.object(kernelcheck, "CACHE", self.cache),
            mock.patch.object(kernelcheck, "load_sources", return_value={}),
            mock.patch.object(kernelcheck, "discover_targets", return_value=(self.target,)),
            mock.patch.object(
                kernelcheck,
                "target_context",
                return_value=(self.target_config, self.platform, self.source, state),
            ),
            mock.patch.object(kernelcheck, "target_source", side_effect=target_source),
            mock.patch.object(kernelcheck, "projected_sources", return_value=[self.projected]),
            mock.patch.object(
                kernelcheck,
                "sparse_targets",
                return_value=["drivers/test-a.o", "drivers/test-b.o"],
            ),
            mock.patch(
                "fplinux_cli.kernelcheck.linux_state.require_prepared_linux",
                return_value=state,
            ),
            mock.patch.object(kernelcheck, "run", side_effect=run_command),
            mock.patch.object(kernelcheck, "run_checkpatch"),
            mock.patch.object(kernelcheck, "run_dtbs_check", side_effect=run_dtbs_check),
        ):
            kernelcheck.check_contexts(None)

    def _cache_path(self, name: str) -> Path:
        """Return one path inside this test's fixed Sparse cache directory."""
        return self.cache / "analysis" / "sparse" / self.target / name

    def test_each_invocation_discards_stale_analyzer_work(self) -> None:
        """A new check cannot inherit object files from an earlier analyzer run."""
        self._run_check()
        stale = self._cache_path("work") / "drivers" / "test-a.o"
        stale.parent.mkdir(parents=True)
        stale.write_text("stale object\n")

        self._run_check()
        self.assertFalse(stale.exists())


if __name__ == "__main__":
    unittest.main()

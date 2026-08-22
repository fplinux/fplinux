# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for kernel-check status handling and command orchestration.

The orchestration tests replace every external analyzer with a command-recording stub.
They verify generated argv and cleanup behavior; they do not run Clang, checkpatch,
Kbuild, dtbs_check, Sparse, a cross-compiler, or a kernel build.
"""

from __future__ import annotations

import signal
import subprocess
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
        result = subprocess.CompletedProcess(["checkpatch"], -signal.SIGTERM, "", "")
        with (
            mock.patch.object(kernelcheck, "capture_text", return_value=result) as capture_text,
            mock.patch.object(kernelcheck, "record_text") as record_text,
            self.assertRaises(SystemExit) as raised,
        ):
            kernelcheck.run_checkpatch(["checkpatch"])
        self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)
        capture_text.assert_called_once_with(["checkpatch"])
        record_text.assert_called_once_with(f"checkpatch exited {-signal.SIGTERM}\n")

    def test_dtbs_signal_uses_shell_status(self) -> None:
        """Convert a signalled dtbs_check status before propagating it."""
        result = subprocess.CompletedProcess(["dtbs_check"], -signal.SIGKILL, "", "")
        with (
            mock.patch.object(kernelcheck, "capture_text", return_value=result) as capture_text,
            mock.patch.object(kernelcheck, "record_text") as record_text,
            self.assertRaises(SystemExit) as raised,
        ):
            kernelcheck.run_dtbs_check(["dtbs_check"], "test-target")
        self.assertEqual(raised.exception.code, 128 + signal.SIGKILL)
        capture_text.assert_called_once_with(["dtbs_check"])
        record_text.assert_called_once_with(f"dtbs_check exited {-signal.SIGKILL}: test-target\n")

    def test_main_runs_the_check_phase(self) -> None:
        """Dispatch the check phase to the analyzer without an inner cache option."""
        reporter = object()
        with (
            mock.patch.object(sys, "argv", ["kernelcheck", "check"]),
            mock.patch(
                "fplinux_cli.kernelcheck.RunReporter.from_environment",
                return_value=reporter,
            ) as from_environment,
            mock.patch.object(kernelcheck, "check_contexts") as check_contexts,
        ):
            kernelcheck.main()
        from_environment.assert_called_once_with("check", "kernel-check")
        check_contexts.assert_called_once_with(reporter)


class KernelAnalyzerOrchestrationWithCommandStubsTests(unittest.TestCase):
    """Check exact analyzer argv while replacing all external commands with stubs."""

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

    def _run_check(
        self,
        *,
        fail_command: str | None = None,
    ) -> list[list[str]]:
        """Run the check flow with command stand-ins and a fixed temporary cache."""
        commands: list[list[str]] = []
        output = self._cache_path("work")
        state = self.prepared_linux

        def run_command(command: list[str]) -> None:
            commands.append(command)
            if fail_command is not None and fail_command in command:
                raise subprocess.CalledProcessError(1, command)
            if command[-1:] == ["savedefconfig"]:
                (output / "defconfig").write_text(self.defconfig.read_text())

        def run_checkpatch(command: list[str]) -> None:
            commands.append(command)

        def run_dtbs_check(command: list[str], _target: str) -> str:
            commands.append(command)
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
            mock.patch.object(kernelcheck, "run_checkpatch", side_effect=run_checkpatch),
            mock.patch.object(kernelcheck, "run_dtbs_check", side_effect=run_dtbs_check),
        ):
            kernelcheck.check_contexts(None)
        return commands

    def _cache_path(self, name: str) -> Path:
        """Return one path inside this test's fixed Sparse cache directory."""
        return self.cache / "analysis" / "sparse" / self.target / name

    def test_each_invocation_emits_the_exact_analyzer_command_sequence(self) -> None:
        """Record all stubbed commands and require the complete ordered argv contract."""
        first = self._run_check()
        stale = self._cache_path("work") / "drivers" / "test-a.o"
        stale.parent.mkdir(parents=True)
        stale.write_text("stale object\n")

        second = self._run_check()

        output = self._cache_path("work")
        kbuild = [
            "make",
            "-C",
            str(self.source),
            f"O={output}",
            "ARCH=arm",
            "CROSS_COMPILE=arm-linux-gnueabihf-",
        ]
        expected = [
            [
                "clang-format",
                f"--style=file:{self.source / '.clang-format'}",
                "--dry-run",
                "--Werror",
                str(self.projected),
            ],
            [
                str(self.source / "scripts/checkpatch.pl"),
                f"--root={self.source}",
                "--terse",
                "-f",
                str(self.projected),
            ],
            [*kbuild, "olddefconfig", "prepare"],
            [*kbuild, "savedefconfig"],
            [*kbuild, "W=1", "dtbs_check"],
            [
                *kbuild,
                "-j1",
                "W=1e",
                "C=2",
                "CHECK=sparse",
                "CF=-D__CHECK_ENDIAN__ -Wsparse-error",
                "drivers/test-a.o",
                "drivers/test-b.o",
            ],
        ]

        self.assertEqual(first, expected)
        self.assertEqual(second, expected)
        self.assertFalse(stale.exists())
        self.assertFalse((self._cache_path("success.json")).exists())

    def test_stubbed_kconfig_failure_creates_no_inner_success_receipt(self) -> None:
        """A stubbed command failure cannot leave an analyzer-owned success marker."""
        with self.assertRaises(subprocess.CalledProcessError):
            self._run_check(fail_command="olddefconfig")

        self.assertFalse(self._cache_path("success.json").exists())


if __name__ == "__main__":
    unittest.main()

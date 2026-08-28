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
from typing import Any
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
        self.target_config: dict[str, Any] = {
            "identity": {
                "display_name": "Test target",
                "compatible": "test,target",
            },
            "linux": {
                "dtb": "test-target.dtb",
                "patches": [],
                "copies": [],
                "appends": [],
                "root": {"kind": "initramfs"},
            },
        }
        self.platform: dict[str, Any] = {
            "identity": {"compatible": "test,soc"},
            "linux": {
                "arch": "arm",
                "analysis_cross_compile": "arm-linux-gnueabihf-",
                "config_script": "scripts/config",
                "dtb_output_directory": "arch/arm/boot/dts",
                "patches": [],
            },
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
            mock.patch.object(kernelcheck, "discover_profiles", return_value=()),
            mock.patch.object(
                kernelcheck,
                "target_context",
                return_value=(self.target_config, self.platform, self.source, state),
            ),
            mock.patch.object(kernelcheck, "target_source", side_effect=target_source),
            mock.patch.object(kernelcheck, "target_defconfig_path", return_value=self.defconfig),
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
            mock.patch.object(kernelcheck, "verify_target_identity"),
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

    def test_profile_applies_actions_without_comparing_the_base_defconfig(self) -> None:
        """A profile check uses its own effective .config, not the default canonical file."""
        profile = "host"
        output = self.cache / "analysis/sparse/test-target/profiles/host/work"
        config_script = self.source / "scripts/config"
        config_script.write_text("#!/bin/sh\n")
        target_config = {
            "identity": {
                "display_name": "Test target",
                "compatible": "test,target",
            },
            "linux": {
                "dtb": "test-target.dtb",
                "patches": [],
                "copies": [],
                "appends": [],
                "config_enable": ["CONFIG_PROFILE_ENABLED"],
                "config_disable": ["CONFIG_PROFILE_DISABLED"],
                "root": {"kind": "initramfs"},
            },
        }
        calls: list[list[str]] = []

        def run_command(command: list[str]) -> None:
            calls.append(command)
            if command[0] == str(config_script):
                (output / ".config").write_text(
                    "CONFIG_TEST=y\nCONFIG_PROFILE_ENABLED=y\n"
                    "# CONFIG_PROFILE_DISABLED is not set\n"
                )
            if command[-1:] == ["savedefconfig"]:
                self.fail("profile configuration must not be compared with base defconfig")

        def target_source(_target: str, _relative: str) -> Path:
            return self.defconfig

        with (
            mock.patch.object(kernelcheck, "CACHE", self.cache),
            mock.patch.object(kernelcheck, "load_sources", return_value={}),
            mock.patch.object(kernelcheck, "discover_targets", return_value=(self.target,)),
            mock.patch.object(kernelcheck, "discover_profiles", return_value=(profile,)),
            mock.patch.object(
                kernelcheck,
                "target_context",
                return_value=(target_config, self.platform, self.source, self.prepared_linux),
            ),
            mock.patch.object(kernelcheck, "target_source", side_effect=target_source),
            mock.patch.object(kernelcheck, "target_defconfig_path", return_value=self.defconfig),
            mock.patch.object(kernelcheck, "projected_sources", return_value=[self.projected]),
            mock.patch.object(kernelcheck, "sparse_targets", return_value=["drivers/test-a.o"]),
            mock.patch(
                "fplinux_cli.kernelcheck.linux_state.require_prepared_linux",
                return_value=self.prepared_linux,
            ),
            mock.patch.object(kernelcheck, "run", side_effect=run_command),
            mock.patch.object(kernelcheck, "run_checkpatch"),
            mock.patch.object(kernelcheck, "run_dtbs_check", return_value=""),
            mock.patch.object(kernelcheck, "verify_target_identity"),
        ):
            kernelcheck.check_contexts(None, profile)

        profile_command = [
            str(config_script),
            "--file",
            str(output / ".config"),
            "--enable",
            "PROFILE_ENABLED",
            "--disable",
            "PROFILE_DISABLED",
        ]
        self.assertIn(profile_command, calls)
        self.assertFalse(any(command[-1:] == ["savedefconfig"] for command in calls))


class KernelProfileSelectionTests(unittest.TestCase):
    """Keep default and explicitly named kernel contexts separate."""

    def test_default_ignores_declared_profiles_and_named_is_narrow(self) -> None:
        """Only an explicit name selects declared profile contexts."""
        profiles = {"first": ("host",), "second": ("host", "diagnostic")}
        with (
            mock.patch.object(kernelcheck, "discover_targets", return_value=("first", "second")),
            mock.patch.object(
                kernelcheck,
                "discover_profiles",
                side_effect=AssertionError("default kernel check must not inspect profiles"),
            ),
        ):
            self.assertEqual(
                kernelcheck.target_profiles(),
                (
                    ("first", None),
                    ("second", None),
                ),
            )

        with (
            mock.patch.object(kernelcheck, "discover_targets", return_value=("first", "second")),
            mock.patch.object(
                kernelcheck,
                "discover_profiles",
                side_effect=lambda target: profiles[target],
            ),
        ):
            self.assertEqual(
                kernelcheck.target_profiles("host"),
                (("first", "host"), ("second", "host")),
            )

    def test_unknown_profile_fails_before_analyzer_work(self) -> None:
        """An unknown profile cannot start an analyzer or create its cache slot."""
        with (
            mock.patch.object(kernelcheck, "discover_targets", return_value=("first",)),
            mock.patch.object(kernelcheck, "discover_profiles", return_value=()),
            self.assertRaisesRegex(SystemExit, "not declared by any target"),
        ):
            kernelcheck.target_profiles("missing")


if __name__ == "__main__":
    unittest.main()

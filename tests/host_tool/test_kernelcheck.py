# SPDX-License-Identifier: GPL-2.0-only
"""Exercise analyzer target selection with a small GNU Make/CC fixture.

The fixture models configuration-selected objects. It does not run
Linux Kbuild or Sparse; the public kernel check covers those tools.
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from fplinux_cli import kernelcheck


class AnalyzerDriverSelectionTests(unittest.TestCase):
    """Leave configured source selection to the external build tool."""

    def setUp(self) -> None:
        """Create one enabled driver and one deliberately uncompilable disabled driver."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.drivers = self.root / "drivers" / "sensors"
        self.drivers.mkdir(parents=True)
        (self.root / "scripts").mkdir()
        (self.root / "include" / "config").mkdir(parents=True)
        (self.root / "include" / "config" / "auto.conf").write_text("CONFIG_CAMERA_A=y\n")
        (self.root / "scripts" / "Makefile.build").write_text(
            "include $(objtree)/include/config/auto.conf\n"
            "real-obj-y := $(if $(filter y,$(CONFIG_CAMERA_A)),$(obj)/camera_a.o)\n"
        )
        (self.root / "config.mk").write_text("CONFIG_CAMERA_A=y\n")
        (self.root / "Makefile").write_text(
            "include config.mk\n"
            "enabled := $(if $(filter y,$(CONFIG_CAMERA_A)),drivers/sensors/camera_a.o)\n"
            ".PHONY: drivers/sensors/\n"
            "drivers/sensors/: $(enabled)\n"
            "drivers/sensors/%.o: drivers/sensors/%.c\n"
            "\t$(CC) -c $< -o $@\n"
        )
        (self.drivers / "camera_a.c").write_text("int camera_a;\n")
        (self.drivers / "camera_b.c").write_text("#error disabled camera must not compile\n")

    def _build_selected_drivers(self) -> subprocess.CompletedProcess[str]:
        """Pass production-selected targets to real Make and the host C compiler."""
        return subprocess.run(
            [
                "make",
                "--no-print-directory",
                "-C",
                str(self.root),
                "CC=cc",
                *kernelcheck.sparse_build_targets(
                    self.root,
                    self.root,
                    ["drivers/sensors/camera_a.o", "drivers/sensors/camera_b.o"],
                    arch="arm",
                    cross_compile="",
                ),
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )

    def test_disabled_driver_does_not_fail_the_selected_build(self) -> None:
        """A disabled driver's missing dependencies cannot break another target's check."""
        result = self._build_selected_drivers()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.drivers / "camera_a.o").is_file())
        self.assertFalse((self.drivers / "camera_b.o").exists())

    def test_enabled_driver_compile_error_still_fails_the_selected_build(self) -> None:
        """Configuration-aware selection must retain failures in enabled code."""
        (self.drivers / "camera_a.c").write_text("#error enabled camera is broken\n")
        result = self._build_selected_drivers()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.drivers / "camera_a.o").exists())

    def test_failed_object_selection_cannot_report_a_successful_check(self) -> None:
        """A failed Make query must stop analysis rather than silently omit drivers."""
        (self.root / "scripts" / "Makefile.build").write_text(
            "$(error cannot resolve configured drivers)\n"
        )
        with self.assertRaises(SystemExit):
            self._build_selected_drivers()
        self.assertFalse((self.drivers / "camera_a.o").exists())

# SPDX-License-Identifier: GPL-2.0-only
"""Exercise analyzer target selection against a small GNU Make fixture.

The fixture models a configuration-selected object list. It does not run
Linux Kbuild or Sparse; the public kernel check covers those tools.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli import kernelcheck


class AnalyzerDriverSelectionTests(unittest.TestCase):
    """Leave configured source selection to the external build tool."""

    def setUp(self) -> None:
        """Configure one of two projected drivers through a Kbuild-like Makefile."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "scripts").mkdir()
        (self.root / "include" / "config").mkdir(parents=True)
        (self.root / "include" / "config" / "auto.conf").write_text("CONFIG_CAMERA_A=y\n")
        (self.root / "scripts" / "Makefile.build").write_text(
            "include $(objtree)/include/config/auto.conf\n"
            "real-obj-y := $(if $(filter y,$(CONFIG_CAMERA_A)),$(obj)/camera_a.o)\n"
            "real-obj-y += $(if $(filter y,$(CONFIG_CAMERA_B)),$(obj)/camera_b.o)\n"
        )

    def _select_drivers(self) -> list[str]:
        """Ask production to select analyzer targets through real Make."""
        return kernelcheck.sparse_build_targets(
            self.root,
            self.root,
            ["drivers/sensors/camera_a.o", "drivers/sensors/camera_b.o"],
            arch="arm",
            cross_compile="",
        )

    def test_analyzer_targets_are_exactly_the_configured_drivers(self) -> None:
        """An unconfigured driver is skipped and a configured one is never dropped."""
        self.assertEqual(self._select_drivers(), ["drivers/sensors/camera_a.o"])

    def test_failed_object_selection_cannot_report_a_successful_check(self) -> None:
        """A failed Make query must stop analysis rather than silently omit drivers."""
        (self.root / "scripts" / "Makefile.build").write_text(
            "$(error cannot resolve configured drivers)\n"
        )
        with self.assertRaises(SystemExit):
            self._select_drivers()

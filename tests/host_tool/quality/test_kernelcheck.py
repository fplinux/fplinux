# SPDX-License-Identifier: GPL-2.0-only
"""Exercise analyzer target selection against a small GNU Make fixture.

The fixture models a configuration-selected object list. It does not run
Linux Kbuild or Sparse; the public kernel check covers those tools.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from fplinux_cli.quality.kernel import analysis as kernel_analysis

FIXTURES = Path(__file__).parents[2] / "fixtures/quality/host_tool/analyzer"


@pytest.fixture
def analyzer_root(tmp_path: Path) -> Path:
    """Configure one of two projected drivers through a Kbuild-like Makefile."""
    (tmp_path / "scripts").mkdir()
    (tmp_path / "include/config").mkdir(parents=True)
    (tmp_path / "include/config/auto.conf").write_text("CONFIG_CAMERA_A=y\n")
    (tmp_path / "scripts/Makefile.build").write_bytes(
        (FIXTURES / "Makefile.build.fixture").read_bytes()
    )
    return tmp_path


def select_drivers(root: Path) -> list[str]:
    """Ask production to select analyzer targets through real Make."""
    return kernel_analysis.sparse_build_targets(
        root,
        root,
        ["drivers/sensors/camera_a.o", "drivers/sensors/camera_b.o"],
        arch="arm",
        cross_compile="",
    )


class AnalyzerDriverSelectionTests:
    """Leave configured source selection to the external build tool."""

    def test_analyzer_targets_are_exactly_the_configured_drivers(
        self, analyzer_root: Path
    ) -> None:
        """An unconfigured driver is skipped and a configured one is never dropped."""
        assert select_drivers(analyzer_root) == ["drivers/sensors/camera_a.o"]

    def test_failed_object_selection_cannot_report_a_successful_check(
        self, analyzer_root: Path
    ) -> None:
        """A failed Make query must stop analysis rather than silently omit drivers."""
        (analyzer_root / "scripts/Makefile.build").write_text(
            "$(error cannot resolve configured drivers)\n"
        )
        with pytest.raises(SystemExit):
            select_drivers(analyzer_root)

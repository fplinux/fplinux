# SPDX-License-Identifier: GPL-2.0-only
"""FM driver callback checks with synthetic firmware and V4L2 registration."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

KERNEL = ROOT / "platforms/ums9117/linux/drivers/bluetooth/ums9117"
FIXTURES = ROOT / "tests/host_tool/ums9117"


class Ums9117FMHostTests:
    """Link the production driver; no V4L2 core or hardware is exercised."""

    def test_close_restores_clock_or_retains_quarantined_hold(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Callbacks preserve RF bits, validate firmware replies and retain failed holds."""
        with c_build_directory(tmp_path_factory, "ums9117_fm") as temporary:
            executable = temporary / "fm"
            compile_c(
                executable,
                [
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wno-unused-parameter",
                    "-Wno-sign-compare",
                    "-UNDEBUG",
                    "-DCONFIG_RADIO_UMS9117_CM4=1",
                    f"-I{FIXTURES / 'fm-compat'}",
                    f"-I{FIXTURES / 'bluetooth-compat'}",
                    f"-I{KERNEL}",
                    str(FIXTURES / "ums9117-fm.c"),
                    str(KERNEL / "cm4-fm.c"),
                ],
                name="compile UMS9117 FM driver host callbacks",
                check=True,
            )
            run_process(
                [str(executable)],
                name="run UMS9117 FM driver host callbacks",
                timeout=10,
                check=True,
            )

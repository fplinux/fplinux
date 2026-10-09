# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked headphone calibration with controlled register and time boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

FIXTURES = ROOT / "tests/host_tool/ums9117"
DRIVER = ROOT / "platforms/ums9117/linux/sound/drivers/ums9117"


class CodecCalibrationHostComponentTests:
    """Observe the real operation's result and masked register restoration on a host."""

    def test_route_preservation_bounded_waits_and_error_precedence(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Snapshot failure has no writes; late errors restore routes and keep first error."""
        with c_build_directory(tmp_path_factory, "codec_calibration") as temporary:
            executable = temporary / "codec-calibration"
            compile_c(
                executable,
                [
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{FIXTURES / 'native-operation-compat/codec'}",
                    f"-I{FIXTURES / 'bluetooth-compat'}",
                    f"-I{DRIVER}",
                    str(FIXTURES / "codec-calibration.c"),
                    str(DRIVER / "ums9117-sc2720-calibration.c"),
                ],
                name="compile SC2720 calibration host component",
                check=True,
            )
            run_process(
                [str(executable)],
                name="check SC2720 calibration results and register restoration",
                timeout=10,
                check=True,
            )

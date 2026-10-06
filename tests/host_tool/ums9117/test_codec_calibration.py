# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked headphone calibration with controlled register and time boundaries."""

from __future__ import annotations

import tempfile
from pathlib import Path

from tests import ROOT
from tests.process import run_process

FIXTURES = ROOT / "tests/host_tool/ums9117"
DRIVER = ROOT / "platforms/ums9117/linux/sound/drivers/ums9117"


class CodecCalibrationHostComponentTests:
    """Observe the real operation's result and masked register restoration on a host."""

    def test_route_preservation_bounded_waits_and_error_precedence(self) -> None:
        """Snapshot failure has no writes; late errors restore routes and keep first error."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "codec-calibration"
            run_process(
                [
                    "cc",
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
                    "-o",
                    str(executable),
                ],
                name="compile SC2720 calibration host component",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="check SC2720 calibration results and register restoration",
                timeout=10,
                check=True,
            )

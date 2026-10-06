# SPDX-License-Identifier: GPL-2.0-only
"""Host-component tests for the shared UMS9117 SDIO controller core."""

from __future__ import annotations

import tempfile
from pathlib import Path

from tests import ROOT
from tests.process import run_process

UBOOT = ROOT / "platforms/ums9117/uboot"
HARNESS = ROOT / "tests/host_tool/ums9117/ums9117-sdio-core.c"
COMPAT = ROOT / "tests/host_tool/ums9117/ums9117-sdio-compat"


class Ums9117SdioCoreTests:
    """Link the production core to a fake MMIO/timer hardware boundary."""

    def test_controller_sequences_and_fail_closed_boundaries(self) -> None:
        """Preserve proven writes and stop after named reset/status failures."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-sdio-core"
            run_process(
                [
                    "cc",
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPAT}",
                    f"-I{UBOOT}",
                    str(HARNESS),
                    str(UBOOT / "ums9117-sdio-core.c"),
                    "-o",
                    str(executable),
                ],
                name="compile UMS9117 SDIO core harness",
                timeout=30,
                check=True,
            )
            result = run_process(
                [str(executable)],
                name="run UMS9117 SDIO core harness",
                timeout=30,
            )
        assert (result.returncode) == (0), f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"

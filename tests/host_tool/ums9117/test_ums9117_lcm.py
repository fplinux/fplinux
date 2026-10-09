# SPDX-License-Identifier: GPL-2.0-only
"""Host LCM transport and RGB565 damage checks with fake MMIO and kernel types."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

KERNEL = ROOT / "platforms/ums9117/linux/drivers/gpu/drm/ums9117"
FIXTURES = ROOT / "tests/host_tool/ums9117"


class Ums9117LcmHostTests:
    """Link the real transport and internal header; no panel is exercised."""

    def test_timing_busy_buffer_and_rgb565_damage_alignment(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Encode timing, wait for port admission and preserve aligned damage."""
        with c_build_directory(tmp_path_factory, "ums9117_lcm") as temporary:
            executable = temporary / "ums9117-lcm"
            compilation = compile_c(
                executable,
                [
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-ffunction-sections",
                    "-fdata-sections",
                    f"-I{FIXTURES / 'lcm-compat'}",
                    f"-I{KERNEL}",
                    str(FIXTURES / "ums9117-lcm.c"),
                    str(KERNEL / "ums9117-drm-lcm.c"),
                    "-Wl,--gc-sections",
                ],
                name="compile UMS9117 LCM host component",
                check=False,
            )
            assert (compilation.returncode) == (0), compilation.stderr
            result = run_process(
                [str(executable)],
                name="run UMS9117 LCM host component",
                timeout=10,
            )
            assert (result.returncode) == (0), result.stdout + result.stderr

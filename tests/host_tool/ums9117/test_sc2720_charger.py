# SPDX-License-Identifier: GPL-2.0-only
"""Host charger callbacks with fake regmap and power-supply registration."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

FIXTURES = ROOT / "tests/host_tool/ums9117"
DRIVER = ROOT / "platforms/ums9117/linux/drivers/power/supply/sc2720-charger-ums9117.c"


class Sc2720ChargerHostTests:
    """Link the real driver; no kernel or physical charger is exercised."""

    def test_usb_type_online_and_read_only_errors(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Decode status snapshots without writes and preserve callback errors."""
        with c_build_directory(tmp_path_factory, "sc2720_charger") as temporary:
            executable = temporary / "sc2720-charger"
            compilation = compile_c(
                executable,
                [
                    "-O2",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{FIXTURES / 'charger-compat'}",
                    str(FIXTURES / "sc2720-charger.c"),
                    str(DRIVER),
                ],
                name="compile SC2720 charger host component",
                check=False,
            )
            assert (compilation.returncode) == (0), compilation.stderr
            result = run_process(
                [str(executable)],
                name="run SC2720 charger host component",
                timeout=10,
            )
            assert (result.returncode) == (0), result.stdout + result.stderr

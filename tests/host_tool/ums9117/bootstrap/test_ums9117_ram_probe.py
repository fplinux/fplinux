# SPDX-License-Identifier: GPL-2.0-only
"""Host component regression for RAM-probe payload preservation."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import pytest

BOOTSTRAP = ROOT / "platforms/ums9117/bootstrap/src"
HARNESS = ROOT / "tests/host_tool/ums9117/bootstrap/ums9117-ram-probe.c"


class RamProbeTests:
    """Exercise the bootstrap probe against real host words and aliases."""

    def test_probe_preserves_payload_words_and_alias_detection(
        self, tmp_path_factory: pytest.TempPathFactory
    ) -> None:
        """Return the vendor detection result without changing any probe word."""
        with c_build_directory(tmp_path_factory, "ums9117_ram_probe") as temporary:
            executable = temporary / "ums9117-ram-probe"
            compile_c(
                executable,
                [
                    "-std=c99",
                    "-O2",
                    "-pedantic",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{BOOTSTRAP}",
                    str(HARNESS),
                ],
                name="compile RAM-probe harness",
                check=True,
            )
            run_process(
                [str(executable)],
                name="check RAM-probe payload and alias preservation",
                timeout=10,
                check=True,
            )

# SPDX-License-Identifier: GPL-2.0-only
"""Host component test for the shared C11 numeric-keypad multi-tap core."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

CORE = ROOT / "lib/fplinux/fplinux-multitap.c"
HARNESS = ROOT / "tests/host_tool/input/fplinux-multitap-core.c"
INCLUDE = ROOT / "include/fplinux"


class MultiTapCoreTests:
    """Exercise the portable state machine without target hardware."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "multitap-core") as temporary:
            cls.executable = temporary / "fplinux-multitap-core"
            compile_c(
                cls.executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(INCLUDE),
                    str(HARNESS),
                    str(CORE),
                ],
                name="compile multi-tap harness",
                check=True,
            )
            yield

    def test_c11_core_contract(self) -> None:
        """Exact groups and time boundaries remain shared behavior."""
        run_process(
            [str(self.executable)],
            name="run multi-tap harness",
            timeout=10,
            check=True,
        )

# SPDX-License-Identifier: GPL-2.0-only
"""Host component test for the shared C11 numeric-keypad multi-tap core."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

CORE = ROOT / "lib/fplinux/fplinux-multitap.c"
HARNESS = ROOT / "tests/host_tool/input/fplinux-multitap-core.c"
INCLUDE = ROOT / "include/fplinux"


class MultiTapCoreTests:
    """Exercise the portable state machine without target hardware."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            temporary = build_directory.name
            cls.executable = Path(temporary) / "fplinux-multitap-core"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(INCLUDE),
                    str(HARNESS),
                    str(CORE),
                    "-o",
                    str(cls.executable),
                ],
                name="compile multi-tap harness",
                timeout=30,
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

# SPDX-License-Identifier: GPL-2.0-only
"""Host component characterization of the tools' decimal argument syntax."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

SHARED = ROOT / "include/fplinux"
HARNESS = ROOT / "tests/host_tool/runtime/fplinux-parse-unsigned.c"


class ParseUnsignedTests:
    """Compile the shared argument parser into a real host C executable."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "parse-unsigned") as temporary:
            cls.executable = temporary / "parse-unsigned"
            compile_c(
                cls.executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED}",
                    str(HARNESS),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                ],
                name="compile unsigned argument parser harness",
                check=True,
            )
            yield

    def test_decimal_syntax_bounds_and_unchanged_output_on_failure(self) -> None:
        """Preserve numeric syntax, inclusive bounds and failure output values."""
        run_process(
            [str(self.executable)],
            name="check decimal argument syntax and bounds",
            timeout=10,
            check=True,
        )

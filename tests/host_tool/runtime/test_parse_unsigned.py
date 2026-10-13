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


@pytest.fixture(scope="module")
def parse_unsigned_executable(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the host harness once and remove its tree after the module."""
    with c_build_directory(tmp_path_factory, "parse-unsigned") as temporary:
        executable = temporary / "parse-unsigned"
        compile_c(
            executable,
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
        yield executable


class ParseUnsignedTests:
    """Compile the shared argument parser into a real host C executable."""

    def test_decimal_syntax_bounds_and_unchanged_output_on_failure(
        self, parse_unsigned_executable: Path
    ) -> None:
        """Preserve numeric syntax, inclusive bounds and failure output values."""
        run_process(
            [str(parse_unsigned_executable)],
            name="check decimal argument syntax and bounds",
            timeout=10,
            check=True,
        )

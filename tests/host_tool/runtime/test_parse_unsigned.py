# SPDX-License-Identifier: GPL-2.0-only
"""Host component characterization of the tools' decimal argument syntax."""

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

SHARED = ROOT / "include/fplinux"
HARNESS = ROOT / "tests/host_tool/runtime/fplinux-parse-unsigned.c"


class ParseUnsignedTests:
    """Compile the shared argument parser into a real host C executable."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            temporary = build_directory.name
            cls.executable = Path(temporary) / "parse-unsigned"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED}",
                    str(HARNESS),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    "-o",
                    str(cls.executable),
                ],
                name="compile unsigned argument parser harness",
                timeout=30,
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

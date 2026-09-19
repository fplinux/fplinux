# SPDX-License-Identifier: GPL-2.0-only
"""Host component characterization of the tools' decimal argument syntax."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
SHARED = ROOT / "alpine/shared"
HARNESS = ROOT / "tests/host_tool/fplinux-parse-unsigned.c"


class ParseUnsignedTests(unittest.TestCase):
    """Compile the shared argument parser into a real host C executable."""

    def test_decimal_syntax_bounds_and_unchanged_output_on_failure(self) -> None:
        """Preserve numeric syntax, inclusive bounds and failure output values."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "parse-unsigned"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED}",
                    str(HARNESS),
                    str(SHARED / "fplinux-cli.c"),
                    "-o",
                    str(executable),
                ],
                name="compile unsigned argument parser harness",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="check decimal argument syntax and bounds",
                timeout=10,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Observe bridge diagnostics with controlled uinput, channel I/O and waits."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class InputBridgeLoggingTests(unittest.TestCase):
    """The executable owns retries and messages; no host keyboard is created."""

    def test_repeated_failure_is_quiet_until_data_resumes(self) -> None:
        """Report each distinct error once and announce both observed recoveries."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "input-bridge"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(ROOT / "alpine/aports/fplinux-input/fplinux-input.c"),
                    str(ROOT / "tests/host_tool/fplinux-input-io.c"),
                    "-Wl,--wrap=open,--wrap=ioctl,--wrap=poll,--wrap=read,--wrap=sleep",
                    "-o",
                    str(executable),
                ],
                name="compile input bridge with controlled devices",
                timeout=30,
                check=True,
            )
            result = run_process([str(executable)], name="run bridge I/O scenario", timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        errors = result.stderr.splitlines()
        self.assertEqual(len(errors), 3, result.stderr)
        self.assertTrue(all(line.startswith("fplinux-input: ") for line in errors))
        self.assertEqual(sum("cannot open" in line for line in errors), 2)
        self.assertEqual(sum("cannot poll" in line for line in errors), 1)
        self.assertEqual(result.stdout.count("input channel readable"), 2)


if __name__ == "__main__":
    unittest.main()

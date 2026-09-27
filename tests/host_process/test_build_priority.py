# SPDX-License-Identifier: GPL-2.0-only
"""Observe the actual scheduling priority inherited by build subprocesses."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

from tests.process import run_process


class BuildPriorityTests(unittest.TestCase):
    """Builds yield CPU without raising an already lower scheduling priority."""

    def test_worker_and_child_keep_at_least_nice_ten(self) -> None:
        """The native process API applies the default and preserves a caller's nice 15."""
        fixture = Path(__file__).resolve().parents[1] / "fixtures/processes/build_priority.py"
        for initial in (0, 15):
            with self.subTest(initial=initial):
                result = run_process(
                    [sys.executable, str(fixture), str(initial)],
                    name="build worker priority",
                    timeout=10,
                    check=True,
                )
                observed = json.loads(result.stdout)
                expected = max(observed["before"], 10)
                self.assertEqual(observed["after"], expected)
                self.assertEqual(observed["child"], expected)


if __name__ == "__main__":
    unittest.main()

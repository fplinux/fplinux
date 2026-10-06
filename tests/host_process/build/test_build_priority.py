# SPDX-License-Identifier: GPL-2.0-only
"""Observe the nice value that the build priority helper leaves on a real process."""

from __future__ import annotations

import json
import os
import sys
import unittest

from tests import ROOT
from tests.process import python_environment, run_process


class BuildPriorityTests(unittest.TestCase):
    """The helper applies nice 10 without undoing a higher nice value.

    The fixture calls the helper directly; this does not show which commands call it.
    """

    def test_worker_and_child_keep_at_least_nice_ten(self) -> None:
        """A process at nice 0 moves to 10, one at 15 stays, and a later child inherits it."""
        fixture = ROOT / "tests/fixtures/processes/build_priority.py"
        runner_nice = os.getpriority(os.PRIO_PROCESS, 0)
        for initial, expected in ((0, 10), (15, 15)):
            with self.subTest(initial=initial):
                if runner_nice > initial:
                    self.skipTest(
                        f"an unprivileged process at nice {runner_nice} cannot start at {initial}"
                    )
                result = run_process(
                    [sys.executable, str(fixture), str(initial)],
                    name="build worker priority",
                    timeout=10,
                    env=python_environment(),
                    check=True,
                )
                observed = json.loads(result.stdout)
                self.assertEqual(observed["before"], initial)
                self.assertEqual(observed["after"], expected)
                self.assertEqual(observed["child"], expected)


if __name__ == "__main__":
    unittest.main()

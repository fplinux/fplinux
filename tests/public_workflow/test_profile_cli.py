# SPDX-License-Identifier: GPL-2.0-only
"""Public CLI help tests for boot selectors and profile commands."""

from __future__ import annotations

import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
_PUBLIC_HELP_TIMEOUT_SECONDS = 10


class ProfileCliHelpWorkflowTests(unittest.TestCase):
    """Exercise the repository CLI without resolving a bundle or touching USB."""

    def test_help_exposes_the_microsd_boot_alias_and_global_profile_selector(self) -> None:
        """Run and package expose both supported ways to select the microSD profile."""
        for command in ("run", "package"):
            with self.subTest(command=command):
                result = run_process(
                    [str(ROOT / "fplinux"), command, "--help"],
                    name=f"fplinux {command} help",
                    timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
                    cwd=ROOT,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("--boot {microsd}", result.stdout)
                self.assertIn("--profile NAME", result.stdout)



if __name__ == "__main__":
    unittest.main()

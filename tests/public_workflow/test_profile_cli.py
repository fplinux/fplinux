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

    def test_help_exposes_only_the_public_microsd_boot_name(self) -> None:
        """The public CLI advertises microsd, never its contributor profile name."""
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
                self.assertNotIn("microsd-uboot", result.stdout)

    def test_profile_help_exposes_only_the_scoped_plugin_namespace(self) -> None:
        """Profile-owned commands stay below an explicit target and profile."""
        result = run_process(
            [str(ROOT / "fplinux"), "profile", "--help"],
            name="fplinux profile help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nokia-ta1618", result.stdout)
        self.assertIn("profile", result.stdout.lower())
        self.assertIn("arg", result.stdout.lower())
        self.assertNotIn("nand-backup", result.stdout)


if __name__ == "__main__":
    unittest.main()

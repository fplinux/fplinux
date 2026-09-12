# SPDX-License-Identifier: GPL-2.0-only
"""Public CLI help tests for boot selectors and diagnostic tools."""

from __future__ import annotations

import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
_PUBLIC_HELP_TIMEOUT_SECONDS = 10


class ProfileAndToolCliHelpWorkflowTests(unittest.TestCase):
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

    def test_nand_backup_help_exposes_the_read_only_tool_without_connecting(self) -> None:
        """NAND backup is an explicit target command, not a legacy profile plugin."""
        result = run_process(
            [str(ROOT / "fplinux"), "nand", "backup", "--help"],
            name="fplinux nand backup help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nokia-ta1618", result.stdout)
        self.assertIn("output", result.stdout.lower())
        self.assertIn("--profile NAME", result.stdout)

    def test_bluetooth_prepare_help_exposes_live_and_saved_dump_inputs(self) -> None:
        """Preparation advertises one command for a phone or an existing raw dump."""
        result = run_process(
            [str(ROOT / "fplinux"), "bluetooth", "prepare", "--help"],
            name="fplinux bluetooth prepare help",
            timeout=_PUBLIC_HELP_TIMEOUT_SECONDS,
            cwd=ROOT,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("nokia-ta1618", result.stdout)
        self.assertIn("--from-dump PATH", result.stdout)
        self.assertIn("--jobs N", result.stdout)
        self.assertIn("--offline", result.stdout)


if __name__ == "__main__":
    unittest.main()

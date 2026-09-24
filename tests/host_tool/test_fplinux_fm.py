# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for FM command parsing and observable device cleanup."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "alpine/aports/fplinux-fm/fplinux-fm.c"
BOUNDARY = ROOT / "tests/fixtures/fplinux_fm/boundary.c"


class FPLinuxFMHostToolTests(unittest.TestCase):
    """Run the real command with test-owned V4L2 and ALSA boundaries."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Build the command with a narrow hardware double for this host."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "fplinux-fm"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "tests/fixtures/fplinux_fm"),
                "-I",
                str(ROOT / "include/fplinux"),
                str(SOURCE),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(BOUNDARY),
                "-o",
                str(cls.executable),
            ],
            name="compile fplinux-fm with hardware double",
            timeout=30,
            check=True,
        )

    def run_fm(
        self, arguments: list[str], *, scenario: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        """Run one bounded CLI scenario."""
        environment = {"LC_ALL": "C"}
        if scenario:
            environment[scenario] = "1"
        return run_process(
            [str(self.executable), *arguments],
            name="run fplinux-fm",
            timeout=5,
            env=environment,
        )

    def test_help_and_invalid_frequency_do_not_access_radio(self) -> None:
        """Parsing resolves help and exact 100 kHz steps before opening devices."""
        for arguments in (["--help"], ["scan", "--help"], ["play", "--help"]):
            with self.subTest(arguments=arguments):
                result = self.run_fm(arguments)
                self.assertEqual(result.returncode, 0)
                self.assertIn("Usage:", result.stdout)
                self.assertNotIn("RADIO_OPEN", result.stderr)

        for frequency in ("87.4", "108.1", "98.25", "98.3x", "98."):
            with self.subTest(frequency=frequency):
                result = self.run_fm(["play", frequency])
                self.assertEqual(result.returncode, 2)
                self.assertIn("0.1 MHz steps", result.stderr)
                self.assertNotIn("RADIO_OPEN", result.stderr)

    def test_scan_emits_distinct_candidates_without_audio(self) -> None:
        """Inclusive seek scans both band edges without enabling audio."""
        result = self.run_fm(["scan"])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(
            result.stdout.splitlines(),
            [
                "candidate 87.5 MHz",
                "candidate 88.1 MHz",
                "candidate 93.4 MHz",
                "candidate 108.0 MHz",
            ],
        )
        self.assertNotIn("FM_ON", result.stderr)
        self.assertTrue(result.stderr.endswith("RADIO_CLOSE\n"))

    def test_no_channel_scan_is_bounded(self) -> None:
        """The documented no-channel result ends normally without audio."""
        result = self.run_fm(["scan"], scenario="FM_TEST_NO_CHANNEL")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "No FM candidates found.\n")
        self.assertNotIn("FM_ON", result.stderr)

    def test_signal_mutes_before_disabling_alsa_and_closing_radio(self) -> None:
        """A signal after radio unmute takes the ordered cleanup path."""
        result = self.run_fm(["play", "98.3"], scenario="FM_TEST_SIGNAL")
        self.assertEqual(result.returncode, 143)
        self.assertEqual(
            result.stderr.splitlines()[-5:],
            ["FM_ON", "V4L2_UNMUTE", "V4L2_MUTE", "FM_OFF", "RADIO_CLOSE"],
        )

    def test_decimal_frequency_and_bounded_playback(self) -> None:
        """An exact decimal maps to V4L2 LOW units and stops at the deadline."""
        result = self.run_fm(["play", "98.30", "--seconds=1"], scenario="FM_TEST_EXPECT_983")
        self.assertEqual(result.returncode, 0)
        self.assertIn("Playing 98.3 MHz", result.stdout)
        self.assertEqual(
            result.stderr.splitlines()[-5:],
            ["FM_ON", "V4L2_UNMUTE", "V4L2_MUTE", "FM_OFF", "RADIO_CLOSE"],
        )

    def test_failed_tune_never_enables_audio(self) -> None:
        """A timed out tune closes radio without unmuting either audio path."""
        result = self.run_fm(["play", "98.3"], scenario="FM_TEST_FAIL_TUNE")
        self.assertEqual(result.returncode, 1)
        self.assertIn("VIDIOC_S_FREQUENCY", result.stderr)
        self.assertNotIn("FM_ON", result.stderr)
        self.assertTrue(result.stderr.endswith("RADIO_CLOSE\n"))

    def test_failed_audio_changes_identify_requested_state(self) -> None:
        """A failed route or mute operation names the state that did not apply."""
        cases = (
            ("FM_TEST_FAIL_FM_ON", "set FM Playback Switch on:"),
            ("FM_TEST_FAIL_V4L2_UNMUTE", "set V4L2 audio mute off:"),
            ("FM_TEST_FAIL_V4L2_MUTE", "set V4L2 audio mute on:"),
            ("FM_TEST_FAIL_FM_OFF", "set FM Playback Switch off:"),
        )
        for scenario, message in cases:
            with self.subTest(scenario=scenario):
                result = self.run_fm(["play", "98.3", "--seconds=1"], scenario=scenario)
                self.assertEqual(result.returncode, 1)
                self.assertIn(message, result.stderr)
                self.assertTrue(result.stderr.endswith("RADIO_CLOSE\n"))


if __name__ == "__main__":
    unittest.main()

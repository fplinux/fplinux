# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for headphone-jack output switching at the ALSA control boundary."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "alpine/aports/fplinux-jack/fplinux-jack.c"
FIXTURE = ROOT / "tests/fixtures/fplinux_jack"


class FPLinuxJackHostToolTests(unittest.TestCase):
    """Run the real service against a scripted card from an ALSA control double.

    The double replaces alsa-lib and the kernel driver. It cannot show that
    the phone reports the jack or that a switch sequence is silent.
    """

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Build the service with the ALSA control double for this host."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "fplinux-jack"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(FIXTURE),
                "-I",
                str(ROOT / "include/fplinux"),
                str(SOURCE),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(FIXTURE / "boundary.c"),
                "-o",
                str(cls.executable),
            ],
            name="compile fplinux-jack with ALSA control double",
            timeout=30,
            check=True,
        )

    def run_jack(
        self, *, initial: str, events: str = "", **scenario: str
    ) -> subprocess.CompletedProcess[str]:
        """Run the service until the scripted card disappears."""
        environment = {
            "LC_ALL": "C",
            "JACK_TEST_INITIAL": initial,
            "JACK_TEST_EVENTS": events,
            **scenario,
        }
        return run_process(
            [str(self.executable)],
            name="run fplinux-jack",
            timeout=5,
            env=environment,
        )

    @staticmethod
    def switch_writes(result: subprocess.CompletedProcess[str]) -> list[str]:
        """Return the switch writes that reached the card, in order."""
        return [
            line.removeprefix("TRACE ")
            for line in result.stderr.splitlines()
            if line.startswith("TRACE ")
        ]

    def test_start_selects_the_output_for_the_current_jack_state(self) -> None:
        """At start the new output is enabled before the other is disabled."""
        cases = (
            ("1", ["headphone on", "speaker off"], "headphones connected"),
            ("0", ["speaker on", "headphone off"], "headphones disconnected"),
        )
        for initial, writes, message in cases:
            with self.subTest(initial=initial):
                result = self.run_jack(initial=initial)
                self.assertEqual(self.switch_writes(result), writes)
                self.assertIn(message, result.stdout)

    def test_jack_changes_switch_outputs_and_manual_changes_persist(self) -> None:
        """Only jack changes move playback; a manual speaker change stays."""
        result = self.run_jack(initial="0", events="insert,speaker-on,remove")
        self.assertEqual(
            self.switch_writes(result),
            [
                "speaker on",
                "headphone off",
                "headphone on",
                "speaker off",
                "speaker on",
                "headphone off",
            ],
        )

    def test_without_a_speaker_switch_outputs_are_left_alone(self) -> None:
        """A card without the fitted speaker keeps its manual outputs."""
        result = self.run_jack(initial="1", events="remove,insert", JACK_TEST_NO_SPEAKER="1")
        self.assertEqual(self.switch_writes(result), [])
        self.assertIn("no Speaker Playback Switch control", result.stdout)

    def test_failed_enable_keeps_the_previous_output(self) -> None:
        """Playback is never left without an output when enabling one fails."""
        result = self.run_jack(initial="0", events="insert", JACK_TEST_FAIL_WRITE="headphone on")
        self.assertEqual(
            self.switch_writes(result),
            ["speaker on", "headphone off", "headphone on"],
        )
        self.assertIn("set Headphone Playback Switch on:", result.stderr)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for headphone-jack output switching at the ALSA control boundary."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator
    from pathlib import Path

SOURCE = ROOT / "alpine/aports/fplinux-jack/fplinux-jack.c"
FIXTURE = ROOT / "tests/fixtures/fplinux_jack"


class FPLinuxJackHostToolTests:
    """Run the real service against a scripted card from an ALSA control double.

    The double replaces alsa-lib and the kernel driver. It cannot show that
    the phone reports the jack or that a switch sequence is silent.
    """

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Build the service with the ALSA control double for this host."""
        with c_build_directory(tmp_path_factory, "fplinux-jack") as directory:
            executable = directory / "fplinux-jack"
            compile_c(
                executable,
                [
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
                ],
                name="compile fplinux-jack with ALSA control double",
                check=True,
            )
            yield executable

    def run_jack(
        self, executable: Path, *, initial: str, events: str = "", **scenario: str
    ) -> subprocess.CompletedProcess[str]:
        """Run the service until the scripted card disappears and it fails."""
        environment = {
            "LC_ALL": "C",
            "JACK_TEST_INITIAL": initial,
            "JACK_TEST_EVENTS": events,
            **scenario,
        }
        result = run_process(
            [str(executable)],
            name="run fplinux-jack",
            timeout=5,
            env=environment,
        )
        # A lost card ends the service with a diagnostic rather than a crash.
        assert (result.returncode) == (1), result.stderr
        assert ("fplinux-jack: read ALSA events:") in (result.stderr)
        return result

    @staticmethod
    def switch_writes(result: subprocess.CompletedProcess[str]) -> list[str]:
        """Return the switch writes that reached the card, in order."""
        return [
            line.removeprefix("TRACE ")
            for line in result.stderr.splitlines()
            if line.startswith("TRACE ")
        ]

    @pytest.mark.parametrize(
        ("initial", "writes", "message"),
        [
            ("1", ["headphone on", "speaker off"], "headphones connected"),
            ("0", ["speaker on", "headphone off"], "headphones disconnected"),
        ],
        ids=["1", "0"],
    )
    def test_start_selects_the_output_for_the_current_jack_state(
        self, executable: Path, initial: str, writes: list[str], message: str
    ) -> None:
        """At start the new output is enabled before the other is disabled."""
        result = self.run_jack(executable, initial=initial)
        assert (self.switch_writes(result)) == (writes)
        assert (message) in (result.stdout)

    def test_jack_changes_switch_outputs_and_manual_changes_persist(
        self, executable: Path
    ) -> None:
        """Only jack changes move playback; a manual speaker change stays."""
        result = self.run_jack(executable, initial="0", events="insert,speaker-on,remove")
        assert (self.switch_writes(result)) == (
            [
                "speaker on",
                "headphone off",
                "headphone on",
                "speaker off",
                "speaker on",
                "headphone off",
            ]
        )

    def test_without_a_speaker_switch_outputs_are_left_alone(self, executable: Path) -> None:
        """A card without the fitted speaker keeps its manual outputs."""
        result = self.run_jack(
            executable, initial="1", events="remove,insert", JACK_TEST_NO_SPEAKER="1"
        )
        assert (self.switch_writes(result)) == ([])
        assert ("no Speaker Playback Switch control") in (result.stdout)

    def test_failed_enable_keeps_the_previous_output(self, executable: Path) -> None:
        """Playback is never left without an output when enabling one fails."""
        result = self.run_jack(
            executable, initial="0", events="insert", JACK_TEST_FAIL_WRITE="headphone on"
        )
        assert (self.switch_writes(result)) == (["speaker on", "headphone off", "headphone on"])
        assert ("set Headphone Playback Switch on:") in (result.stderr)

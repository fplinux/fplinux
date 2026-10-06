# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for FM command parsing and observable device cleanup."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Iterator

SOURCE = ROOT / "alpine/aports/fplinux-fm/fplinux-fm.c"
BOUNDARY = ROOT / "tests/fixtures/fplinux_fm/boundary.c"


class FPLinuxFMHostToolTests:
    """Run the real command with test-owned V4L2 and ALSA boundaries."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Build the command with a narrow hardware double for this host."""
        with ExitStack() as cleanup:
            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
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
            yield

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

    @pytest.mark.parametrize(
        ("arguments", "status", "message"),
        [
            pytest.param(["--help"], 0, "Usage:", id="root-help"),
            pytest.param(["scan", "--help"], 0, "Usage:", id="scan-help"),
            pytest.param(["play", "--help"], 0, "Usage:", id="play-help"),
            pytest.param(["play", "87.4"], 2, "0.1 MHz steps", id="below-band"),
            pytest.param(["play", "108.1"], 2, "0.1 MHz steps", id="above-band"),
            pytest.param(["play", "98.25"], 2, "0.1 MHz steps", id="half-step"),
            pytest.param(["play", "98.3x"], 2, "0.1 MHz steps", id="trailing-text"),
            pytest.param(["play", "98."], 2, "0.1 MHz steps", id="incomplete-decimal"),
        ],
    )
    def test_help_and_invalid_frequency_do_not_access_radio(
        self, arguments: list[str], status: int, message: str
    ) -> None:
        """Parsing resolves help and exact 100 kHz steps before opening devices."""
        result = self.run_fm(arguments)
        assert result.returncode == status
        assert message in (result.stdout if status == 0 else result.stderr)
        assert "RADIO_OPEN" not in result.stderr

    def test_scan_emits_distinct_candidates_without_audio(self) -> None:
        """Inclusive seek scans both band edges without enabling audio."""
        result = self.run_fm(["scan"])
        assert (result.returncode) == (0)
        assert (result.stdout.splitlines()) == (
            [
                "candidate 87.5 MHz",
                "candidate 88.1 MHz",
                "candidate 93.4 MHz",
                "candidate 108.0 MHz",
            ]
        )
        assert ("FM_ON") not in (result.stderr)
        assert result.stderr.endswith("RADIO_CLOSE\n")

    def test_no_channel_scan_is_bounded(self) -> None:
        """The documented no-channel result ends normally without audio."""
        result = self.run_fm(["scan"], scenario="FM_TEST_NO_CHANNEL")
        assert (result.returncode) == (0)
        assert (result.stdout) == ("No FM candidates found.\n")
        assert ("FM_ON") not in (result.stderr)

    def test_signal_mutes_before_disabling_alsa_and_closing_radio(self) -> None:
        """A signal after radio unmute takes the ordered cleanup path."""
        result = self.run_fm(["play", "98.3"], scenario="FM_TEST_SIGNAL")
        assert (result.returncode) == (143)
        assert (result.stderr.splitlines()[-5:]) == (
            ["FM_ON", "V4L2_UNMUTE", "V4L2_MUTE", "FM_OFF", "RADIO_CLOSE"]
        )

    def test_decimal_frequency_and_bounded_playback(self) -> None:
        """An exact decimal maps to V4L2 LOW units and stops at the deadline."""
        result = self.run_fm(["play", "98.30", "--seconds=1"], scenario="FM_TEST_EXPECT_983")
        assert (result.returncode) == (0)
        assert ("Playing 98.3 MHz") in (result.stdout)
        assert (result.stderr.splitlines()[-5:]) == (
            ["FM_ON", "V4L2_UNMUTE", "V4L2_MUTE", "FM_OFF", "RADIO_CLOSE"]
        )

    def test_failed_tune_never_enables_audio(self) -> None:
        """A timed out tune closes radio without unmuting either audio path."""
        result = self.run_fm(["play", "98.3"], scenario="FM_TEST_FAIL_TUNE")
        assert (result.returncode) == (1)
        assert ("VIDIOC_S_FREQUENCY") in (result.stderr)
        assert ("FM_ON") not in (result.stderr)
        assert result.stderr.endswith("RADIO_CLOSE\n")

    @pytest.mark.parametrize(
        ("scenario", "message"),
        [
            ("FM_TEST_FAIL_FM_ON", "set FM Playback Switch on:"),
            ("FM_TEST_FAIL_V4L2_UNMUTE", "set V4L2 audio mute off:"),
            ("FM_TEST_FAIL_V4L2_MUTE", "set V4L2 audio mute on:"),
            ("FM_TEST_FAIL_FM_OFF", "set FM Playback Switch off:"),
        ],
        ids=[
            "FM_TEST_FAIL_FM_ON",
            "FM_TEST_FAIL_V4L2_UNMUTE",
            "FM_TEST_FAIL_V4L2_MUTE",
            "FM_TEST_FAIL_FM_OFF",
        ],
    )
    def test_failed_audio_changes_identify_requested_state(
        self, scenario: str, message: str
    ) -> None:
        """A failed route or mute operation names the state that did not apply."""
        result = self.run_fm(["play", "98.3", "--seconds=1"], scenario=scenario)
        assert (result.returncode) == (1)
        assert (message) in (result.stderr)
        assert result.stderr.endswith("RADIO_CLOSE\n")

# SPDX-License-Identifier: GPL-2.0-only
"""The audiod device owner against scripted ALSA controls and jack events."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-audiod"
FIXTURE = ROOT / "tests/host_tool/gui/audio/fixtures/audio-device"


@pytest.fixture(scope="module")
def device(tmp_path_factory: pytest.TempPathFactory) -> Path:
    executable = tmp_path_factory.mktemp("audio-controls") / "device"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffunction-sections",
            "-Wl,--gc-sections",
            f"-I{FIXTURE}",
            f"-I{APORT}",
            f"-I{ROOT / 'include/fplinux'}",
            str(APORT / "audio-device.c"),
            str(FIXTURE / "device.c"),
            str(FIXTURE / "boundary.c"),
        ],
        name="compile audiod device owner with ALSA control double",
    )
    return executable


def run_device(device: Path, **scenario: str) -> subprocess.CompletedProcess[str]:
    return run_process(
        [str(device)],
        name="exercise audiod ALSA controls",
        timeout=5,
        env={"LC_ALL": "C", **scenario},
        check=True,
    )


def writes(result: subprocess.CompletedProcess[str]) -> list[str]:
    return [line.removeprefix("TRACE ") for line in result.stderr.splitlines()]


@pytest.mark.parametrize(
    ("initial", "expected"),
    [("1", ["headphone on", "speaker off"]), ("0", ["speaker on", "headphone off"])],
)
def test_auto_route_enables_new_output_before_disabling_old(
    device: Path, initial: str, expected: list[str]
) -> None:
    result = run_device(device, JACK_TEST_INITIAL=initial)
    assert writes(result) == expected
    assert "init=0 rate=48000 speaker=1 headphones=1" in result.stdout


def test_jack_changes_move_auto_route_and_unrelated_controls_do_not(device: Path) -> None:
    result = run_device(device, JACK_TEST_INITIAL="0", JACK_TEST_EVENTS="insert,speaker-on,remove")
    assert writes(result) == [
        "speaker on",
        "headphone off",
        "headphone on",
        "speaker off",
        "speaker on",
        "headphone off",
    ]
    assert result.stdout.splitlines()[1:3] == ["inserted", "removed"]


def test_headphones_only_card_never_requests_missing_speaker(device: Path) -> None:
    result = run_device(device, JACK_TEST_INITIAL="1", JACK_TEST_NO_SPEAKER="1")
    assert writes(result) == ["headphone on"]
    assert "speaker=0 headphones=1" in result.stdout


def test_failed_enable_does_not_disable_previous_output(device: Path) -> None:
    result = run_device(
        device,
        JACK_TEST_INITIAL="0",
        JACK_TEST_EVENTS="insert",
        JACK_TEST_FAIL_WRITE="headphone on",
    )
    assert writes(result) == ["speaker on", "headphone off", "headphone on"]
    assert result.stdout.endswith("events=-5\n")


def test_qin_card_uses_native_44100_with_fixed_speaker(device: Path) -> None:
    result = run_device(device, JACK_TEST_QIN="1")
    assert "init=0 rate=44100 speaker=1 headphones=0" in result.stdout
    assert writes(result) == []


def test_alarm_uses_speaker_when_selected_headphones_are_known_absent_and_restores_preference(
    device: Path,
) -> None:
    result = run_device(device, JACK_TEST_INITIAL="0", JACK_TEST_ALARM_FALLBACK="1")
    assert writes(result) == [
        "speaker on",
        "headphone off",
        "headphone on",
        "speaker off",
        "speaker on",
        "headphone off",
        "headphone on",
        "speaker off",
    ]
    assert result.stdout.splitlines()[-2:] == [
        "alarm result=0 desired=2 applied=1",
        "restore desired=2 applied=2",
    ]


def test_headphones_only_card_reports_alarm_unavailable_when_jack_is_known_absent(
    device: Path,
) -> None:
    result = run_device(
        device, JACK_TEST_INITIAL="0", JACK_TEST_NO_SPEAKER="1", JACK_TEST_ALARM_FALLBACK="1"
    )
    assert writes(result) == ["headphone on"]
    assert result.stdout.splitlines()[-2:] == [
        "alarm result=-19 desired=2 applied=2",
        "restore desired=2 applied=2",
    ]

# SPDX-License-Identifier: GPL-2.0-only
"""FM frontend parsing, service lease lifetime and silent V4L2 seek."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path


@pytest.fixture(scope="module")
def fm(tmp_path_factory: pytest.TempPathFactory) -> Path:
    executable = tmp_path_factory.mktemp("fplinux-fm") / "fm"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "alpine/aports/fplinux-fm/fplinux-fm.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-cli.c"),
            str(ROOT / "tests/host_tool/media/fixtures/fplinux-fm/boundary.c"),
        ],
        name="compile FM frontend with service and tuner boundaries",
    )
    return executable


def run_fm(fm: Path, arguments: list[str], **scenario: str) -> subprocess.CompletedProcess[str]:
    return run_process(
        [str(fm), *arguments],
        name="run FM frontend",
        timeout=5,
        env={"LC_ALL": "C", **scenario},
    )


@pytest.mark.parametrize(
    ("arguments", "status"),
    [
        (["--help"], 0),
        (["scan", "--help"], 0),
        (["play", "--help"], 0),
        (["play", "87.4"], 2),
        (["play", "108.1"], 2),
        (["play", "98.25"], 2),
        (["play", "98.3x"], 2),
        (["play", "98."], 2),
    ],
)
def test_help_and_invalid_frequency_do_not_contact_service(
    fm: Path, arguments: list[str], status: int
) -> None:
    result = run_fm(fm, arguments)
    assert result.returncode == status
    assert "SERVICE_CONNECT" not in result.stderr


def test_scan_uses_lease_and_emits_distinct_silent_candidates(fm: Path) -> None:
    result = run_fm(fm, ["scan"])
    assert result.returncode == 0
    assert result.stdout.splitlines() == [
        "candidate 87.5 MHz",
        "candidate 88.1 MHz",
        "candidate 93.4 MHz",
        "candidate 108.0 MHz",
    ]
    assert result.stderr.splitlines() == [
        "SERVICE_CONNECT",
        "REQUEST SCAN",
        "RADIO_OPEN",
        "RADIO_CLOSE",
        "LEASE_CLOSE",
    ]


def test_no_channel_scan_is_bounded_and_releases_lease(fm: Path) -> None:
    result = run_fm(fm, ["scan"], FM_TEST_NO_CHANNEL="1")
    assert result.returncode == 0
    assert result.stdout == "No FM candidates found.\n"
    assert result.stderr.endswith("RADIO_CLOSE\nLEASE_CLOSE\n")


def test_decimal_playback_requests_service_frequency_and_releases_on_deadline(fm: Path) -> None:
    result = run_fm(fm, ["play", "98.30", "--seconds=1"], FM_TEST_EXPECT_983="1")
    assert result.returncode == 0
    assert result.stdout == "Playing 98.3 MHz; press Ctrl-C to stop.\n"
    assert result.stderr.splitlines() == ["SERVICE_CONNECT", "REQUEST PLAY 983", "LEASE_CLOSE"]


def test_signal_releases_service_lease(fm: Path) -> None:
    result = run_fm(fm, ["play", "98.3"], FM_TEST_SIGNAL="1")
    assert result.returncode == 143
    assert result.stderr.endswith("LEASE_CLOSE\n")


def test_service_refusal_does_not_open_tuner(fm: Path) -> None:
    result = run_fm(fm, ["play", "98.3"], FM_TEST_SERVICE_ERROR="1")
    assert result.returncode == 1
    assert "audio service:" in result.stderr
    assert "RADIO_OPEN" not in result.stderr
    assert "LEASE_CLOSE" in result.stderr


def test_lost_service_ends_frontend_and_releases_lease(fm: Path) -> None:
    result = run_fm(fm, ["play", "98.3"], FM_TEST_SERVICE_GONE="1")
    assert result.returncode == 1
    assert "wait:" in result.stderr
    assert "LEASE_CLOSE" in result.stderr

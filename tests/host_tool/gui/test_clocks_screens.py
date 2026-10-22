# SPDX-License-Identifier: GPL-2.0-only
"""Actual Clock input and captures with a controlled native session owner."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.gui.png_capture import png_pixels
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "site/scripts/screens"
CAPTURE_STATES = (
    "clocks-home",
    "clocks-digital",
    "clocks-analog",
    "clocks-alarm-edit",
    "clocks-days",
    "clocks-pending",
    "clocks-error",
    "clocks-retry",
    "clocks-alarms",
    "clocks-timer",
    "clocks-duration",
    "clocks-stopwatch",
    "clocks-laps",
    "clocks-offset-picker",
    "clocks-offset-detail",
    "clocks-offset-edited",
    "clocks-world",
    "clocks-world-duplicate",
    "clocks-world-retry-add",
    "clocks-world-retry-delete",
    "clocks-world-tomorrow",
)


def test_fixed_offset_projection_crosses_calendar_and_supported_clock_edges(
    tmp_path: Path,
) -> None:
    """Real model and presentation project civil dates through a controlled formatter."""
    aport = ROOT / "alpine/aports/fplinux-clocks"
    peers = ROOT / "tests/host_tool/gui"
    executable = tmp_path / "clock-presentation"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include'}",
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{aport}",
            str(peers / "clock-presentation-test.c"),
            str(peers / "clock-presentation-format-boundary.c"),
            str(aport / "clock-model.c"),
            str(aport / "clock-presentation.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-lintl",
        ],
        name="compile fixed-offset Clock presentation with a controlled UI formatter",
    )
    result = run_process(
        [str(executable)],
        name="project fixed offsets across calendar and clock boundaries",
        timeout=10,
        check=True,
    )
    assert result.stdout == (
        "clock-presentation: fixed offsets, calendar edges and invalid values passed\n"
    )
    assert result.stderr == ""


@pytest.fixture(scope="module")
def clocks_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[Path]:
    """Build the real app and peer against the shared native UI inputs."""
    with c_build_directory(tmp_path_factory, "clocks-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-clocks.sh"), str(directory), str(ui_host_inputs)],
            name="build the real Clock and native capture peer",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.fixture(scope="module")
def clocks_screens(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, clocks_native: Path
) -> Iterator[tuple[Path, str]]:
    """Capture all profiles through the maintained wrapper and the already built app."""
    with c_build_directory(tmp_path_factory, "clocks-screens") as output:
        result = run_process(
            [
                "sh",
                str(SCREENS / "render-clocks.sh"),
                str(output),
                str(ui_host_inputs),
                str(clocks_native),
            ],
            name="capture the actual Clock through its screenshot entrypoint",
            timeout=240,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(output, Path(log_root) / "clocks-screens", dirs_exist_ok=True)
        assert result.returncode == 0, result.stdout + result.stderr
        yield output, result.stdout


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_clocks_help_precedes_arguments_preferences_and_ui(
    clocks_native: Path, tmp_path: Path, arguments: list[str]
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(clocks_native / "clocks"), *arguments],
        name="Clock help before resource access",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-clocks-display",
        },
        timeout=10,
    )
    assert result.returncode == 0
    assert "Usage: fplinux-clocks" in result.stdout
    assert result.stderr == ""
    assert not state.exists()


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_clocks_rejects_arguments_before_preferences_and_ui(
    clocks_native: Path, tmp_path: Path, argument: str
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(clocks_native / "clocks"), argument],
        name="Clock invalid arguments before resource access",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-clocks-display",
        },
        timeout=10,
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-clocks:" in result.stderr
    assert not state.exists()


@pytest.mark.parametrize(
    ("scenario", "expected"),
    [
        (
            "timer-late-ack",
            (
                "clocks-timer-late-ack: accepted 120s draft survives 90s ACK; "
                "second Save stores 120s\n"
            ),
        ),
        (
            "timer-unchanged-ack",
            "clocks-timer-unchanged-ack: unchanged 90s draft closes on Save ACK\n",
        ),
        (
            "active-confirm-late-ack",
            (
                "clocks-active-late-ack: confirmed 08:00 revision keeps 09:00 draft; "
                "next Save stores 09:00\n"
            ),
        ),
        (
            "active-confirm-unchanged-ack",
            "clocks-active-unchanged-ack: unchanged 08:00 draft closes after confirmation ACK\n",
        ),
        (
            "published-add-retry",
            (
                "clocks-published-retry: accepted ADD exposes its ID before synchronization; "
                "Retry saves its checkpoint\n"
            ),
        ),
        (
            "published-background-durable",
            (
                "clocks-background-durable: unchanged accepted draft saves without mutation; "
                "newer draft stays dirty and edits the same ID\n"
            ),
        ),
        (
            "ordinary-close-draft",
            (
                "clocks-ordinary-close: failed draft survives Stay and Save retry; "
                "Leave permits final close and discards newer draft\n"
            ),
        ),
        (
            "storage-controls",
            (
                "clocks-storage-controls: directory sync EIO keeps navigation, ticking "
                "snapshots and running timer Stop available\n"
            ),
        ),
        (
            "storage-ring-retry",
            (
                "clocks-storage-ring-retry: checkpoint Retry keeps the ringing event and "
                "generation; restart retains the consumed alarm\n"
            ),
        ),
        (
            "missed-timer-localized",
            "clocks-missed-localized: expired timer uses Таймер; named alarm keeps Morning tea\n",
        ),
    ],
    ids=[
        "timer-late-ack",
        "timer-unchanged-ack",
        "active-confirm-late-ack",
        "active-confirm-unchanged-ack",
        "published-add-retry",
        "published-background-durable",
        "ordinary-close-draft",
        "storage-controls",
        "storage-ring-retry",
        "missed-timer-localized",
    ],
)
def test_actual_clocks_acknowledgments_and_localized_missed_events(
    clocks_native: Path, tmp_path: Path, scenario: str, expected: str
) -> None:
    """Real widgets retain accepted edits; a gettext boundary localizes event kinds."""
    result = run_process(
        [
            str(clocks_native / "clocks-screens"),
            "240",
            "320",
            str(clocks_native / "clocks"),
            str(SCREENS / "clocks-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            scenario,
        ],
        name=f"exercise actual Clock {scenario}",
        env={"FPLINUX_CLOCKS_TEST_TRANSLATIONS": "ru"}
        if scenario == "missed-timer-localized"
        else {},
        timeout=20,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == expected


@pytest.mark.parametrize(
    ("suffix", "surface"),
    [("-128x160", (128, 128)), ("", (240, 268)), ("-320x240", (320, 196))],
)
def test_actual_clocks_forms_requests_lifecycle_and_rendering_at_each_profile(
    clocks_screens: tuple[Path, str],
    suffix: str,
    surface: tuple[int, int],
) -> None:
    """Actual shared editors drive date alarms, countdowns, laps and fixed offsets."""
    output, transcript = clocks_screens
    for expected in (
        (
            "clocks-alarm: one TIME and DATE; 2024-02-29 08:45 UTC; "
            "visible Save; stale replies ignored; storage retry retains draft\n"
        ),
        "clocks-repeat: Tue Thu Sat; hidden once date; cancelled draft unchanged\n",
        (
            "clocks-retry: one automatic retry; bounded silence; explicit retry retains "
            "serial and payload; one alarm\n"
        ),
        (
            "clocks-late-ack: inline edit survives ADD acknowledgment; Cancel then EDIT "
            "keeps one alarm\n"
        ),
        "clocks-timer: one duration; 90s; pause, resume and stop\n",
        "clocks-stopwatch: direct lap=2000ms; pause=2750ms; lap history\n",
        (
            "clocks-world: UTC default; +05:30; +05:45; -03:30 yesterday; "
            "-12:00 and +14:00 endpoints; duplicate rejected\n"
        ),
        (
            "clocks-world-edit: +05:30 to +06:30; Cancel and duplicate keep value; "
            "late +07:00 ACK keeps stable ID and order\n"
        ),
        (
            "clocks-world-late-ack: add selects new stable ID after Retry closes; "
            "delete leaves no stale detail\n"
        ),
        (
            "clocks-world-calendar: Jan 31 23:45 +05:45 = Feb 1 05:30; "
            "Dec 31 23:50 +05:45 = Jan 1 05:35; Tomorrow\n"
        ),
        "clocks-reopen: owner retains alarm, paused timer and stopwatch lap\n",
    ):
        assert transcript.count(expected) == 3
    assert transcript.count("clocks-face 00:00:00: glyphs and ") == 3
    for state in ("hidden", "background"):
        assert (
            transcript.count(
                f"clocks-idle {state}: no frames or requests; CPU under 100ms in 1200ms\n"
            )
            == 3
        )
    assert transcript.count("clocks: actual app, shared widgets and model wire completed\n") == 3
    frames = []
    width, height = surface
    for state in CAPTURE_STATES:
        actual_width, actual_height, pixels = png_pixels(output / f"{state}{suffix}.png")
        assert (actual_width, actual_height) == surface
        assert len({pixels[index : index + 3] for index in range(0, len(pixels), 3)}) > 8
        assert f"{state}: {width}x{height}\n" in transcript
        frames.append(pixels)
    assert len(set(frames)) == len(CAPTURE_STATES)

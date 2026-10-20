# SPDX-License-Identifier: GPL-2.0-only
"""CPU pixel oracles and real CLI execution with simulated DRM/GPU/VT and time."""

from __future__ import annotations

import errno
import os
import shlex
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-debug-tools"
PEERS = ROOT / "tests/host_tool/display"


@dataclass(frozen=True)
class DisplayTestBinaries:
    """Production pattern harness and command linked to a fake session boundary."""

    patterns: Path
    command: Path


@pytest.fixture(scope="module")
def display_test(tmp_path_factory: pytest.TempPathFactory) -> Iterator[DisplayTestBinaries]:
    """Build production translation units separately from their host peers."""
    with c_build_directory(tmp_path_factory, "display-test") as directory:
        patterns = directory / "patterns"
        command = directory / "fplinux-display-test"
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{APORT}"]
        compile_c(
            patterns,
            [
                *flags,
                str(PEERS / "fplinux-display-test-pattern.c"),
                str(APORT / "display-test-pattern.c"),
            ],
            name="compile display test pattern host oracles",
        )
        drm_flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read display test session header flags",
                timeout=10,
                check=True,
            ).stdout
        )
        compile_c(
            command,
            [
                *flags,
                f"-I{ROOT / 'include/fplinux'}",
                *drm_flags,
                str(APORT / "fplinux-display-test.c"),
                str(APORT / "display-test-pattern.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(PEERS / "fplinux-display-test-session.c"),
                "-Wl,--wrap=clock_gettime,--wrap=poll",
            ],
            name="compile display test command with simulated DRM session",
        )
        yield DisplayTestBinaries(patterns, command)


def test_rgb565_patterns_preserve_pixels_ids_motion_and_padded_rows(
    display_test: DisplayTestBinaries,
) -> None:
    """Literal pixel values cover orientation, 1-pixel detail and three ID copies."""
    run_process(
        [str(display_test.patterns)],
        name="run display pattern host oracles",
        timeout=5,
        check=True,
    )


def test_help_precedes_invalid_values_and_display_access(
    display_test: DisplayTestBinaries,
) -> None:
    result = run_process(
        [str(display_test.command), "--fps", "invalid", "--help"],
        name="show display test help without opening a session",
        timeout=5,
    )

    assert result.returncode == 0
    assert "--pattern=geometry|gray|ids|motion" in result.stdout
    assert "--duration-ms=N" in result.stdout
    assert "0 is unpaced" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize(
    "arguments",
    [
        ("--pattern", "unknown"),
        ("--frames", "0"),
        ("--duration-ms", "60001"),
        ("--fps", "121"),
        ("--frames", "1", "--frames", "2"),
        ("--renderer", "unknown"),
        ("--verify",),
    ],
    ids=[
        "pattern",
        "zero-frames",
        "duration-bound",
        "fps-bound",
        "duplicate",
        "renderer",
        "verify-needs-gpu",
    ],
)
def test_invalid_arguments_fail_before_display_access(
    display_test: DisplayTestBinaries, arguments: tuple[str, ...]
) -> None:
    result = run_process(
        [str(display_test.command), *arguments],
        name="reject display test arguments before opening a session",
        timeout=5,
    )

    assert result.returncode == 2
    assert result.stdout == ""
    assert "--help' for more information." in result.stderr
    assert "fake-session" not in result.stderr


def test_unpaced_frames_report_completed_timestamps_and_restore_session(
    display_test: DisplayTestBinaries,
) -> None:
    result = run_process(
        [str(display_test.command), "--pattern=ids", "--frames=3", "--fps=0"],
        name="run three unpaced display test frames",
        timeout=5,
    )

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert lines[0] == "frame_id,draw_start_ns,submit_ns,complete_ns"
    samples = [[int(value) for value in line.split(",")] for line in lines[1:4]]
    assert [sample[0] for sample in samples] == [0, 1, 2]
    for _, draw_start_ns, submit_ns, complete_ns in samples:
        assert 0 < draw_start_ns <= submit_ns <= complete_ns
    assert samples[0][3] <= samples[1][1] <= samples[1][3] <= samples[2][1]
    assert lines[4].startswith("summary completed=3 elapsed_ns=")
    assert lines[4].endswith(" interrupted=no")
    assert result.stderr == (
        "fake-session opened\nfplinux-display-test: renderer=software\n"
        "fake-session restored presentations=3\n"
    )


@pytest.mark.parametrize("pattern", ["gray", "geometry"])
def test_static_patterns_present_once_and_hold_until_duration(
    display_test: DisplayTestBinaries, pattern: str
) -> None:
    result = run_process(
        [
            str(display_test.command),
            f"--pattern={pattern}",
            "--frames=1",
            "--duration-ms=2000",
        ],
        name="hold a static optical control without repeated presentation",
        timeout=5,
    )

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert len(lines) == 3
    assert lines[1].startswith("0,")
    assert lines[2].startswith("summary completed=1 ")
    elapsed_ns = int(lines[2].split("elapsed_ns=", 1)[1].split(" ", 1)[0])
    assert elapsed_ns >= 2_000_000_000
    assert result.stderr == (
        "fake-session opened\nfplinux-display-test: renderer=software\n"
        "fake-session restored presentations=1\n"
    )


@pytest.mark.parametrize("scenario", ["normal", "inactive"])
def test_wall_clock_limit_ends_pacing_and_inactive_vt_waits(
    display_test: DisplayTestBinaries, scenario: str
) -> None:
    result = run_process(
        [
            str(display_test.command),
            f"--drm={scenario}",
            "--pattern=ids",
            "--duration-ms=20",
            "--frames=65536",
            "--fps=1",
        ],
        name="stop display diagnostic at its wall-clock limit",
        timeout=2,
    )

    assert result.returncode == 0, result.stderr
    expected_frames = 1 if scenario == "normal" else 0
    assert f"summary completed={expected_frames} " in result.stdout
    assert result.stderr == (
        "fake-session opened\nfplinux-display-test: renderer=software\n"
        f"fake-session restored presentations={expected_frames}\n"
    )


@pytest.mark.parametrize("scenario", ["invalid-layout", "present-error"])
def test_layout_or_presentation_failure_restores_the_session(
    display_test: DisplayTestBinaries, scenario: str
) -> None:
    result = run_process(
        [str(display_test.command), f"--drm={scenario}", "--frames=1", "--fps=0"],
        name="restore display session after a diagnostic failure",
        timeout=5,
    )

    assert result.returncode == 1
    assert "fplinux-display-test: presentation loop:" in result.stderr
    assert result.stderr.startswith("fake-session opened\n")
    presentations = 0 if scenario == "invalid-layout" else 1
    assert result.stderr.endswith(f"fake-session restored presentations={presentations}\n")
    assert "0," not in result.stdout


def test_stop_signal_restores_session_and_retains_signal_status(
    display_test: DisplayTestBinaries,
) -> None:
    result = run_process(
        [str(display_test.command), "--drm=signal", "--frames=3", "--fps=0"],
        name="stop display diagnostic during presentation",
        timeout=5,
    )

    assert result.returncode == 143
    assert "summary completed=1 " in result.stdout
    assert result.stdout.endswith(" interrupted=yes\n")
    assert result.stderr == (
        "fake-session opened\nfplinux-display-test: renderer=software\n"
        "fake-session restored presentations=1\n"
    )


@pytest.mark.parametrize("verify", [False, True], ids=["timing", "pixel-verification"])
def test_gpu_selection_uses_gpu_frames_and_closes_before_the_borrowed_session(
    display_test: DisplayTestBinaries, *, verify: bool
) -> None:
    """The GPU double checks CLI dispatch and ownership, not shader correctness."""
    result = run_process(
        [
            str(display_test.command),
            "--renderer=gpu",
            "--pattern=motion",
            "--frames=3",
            "--fps=0",
            *(["--verify"] if verify else []),
        ],
        name="route display frames through the selected GPU backend",
        timeout=5,
    )

    assert result.returncode == 0, result.stderr
    assert "summary completed=3 " in result.stdout
    assert "renderer=gpu" in result.stderr
    assert "renderer=software" not in result.stderr
    checks = 3 if verify else 0
    assert result.stderr.endswith(
        f"fake-gpu restored draws=3 checks={checks}\nfake-session restored presentations=3\n"
    )


@pytest.mark.parametrize(
    ("scenario", "verify", "diagnostic", "presentations"),
    [
        ("gpu-open-error", False, "controlled GPU startup failure", 0),
        ("gpu-draw-error", False, "controlled GPU draw failure", 0),
        ("present-error", False, "controlled GPU completion failure", 1),
        ("gpu-verify-error", True, "controlled GPU pixel mismatch", 0),
        ("gpu-close-error", False, "GPU cleanup", 1),
        ("gpu-wait-error", False, f"presentation loop: {os.strerror(errno.EIO)}", 1),
    ],
    ids=["startup", "draw", "queue", "pixels", "cleanup", "completion"],
)
def test_gpu_failures_remain_errors_and_restore_the_display(
    display_test: DisplayTestBinaries,
    scenario: str,
    diagnostic: str,
    presentations: int,
    *,
    verify: bool,
) -> None:
    result = run_process(
        [
            str(display_test.command),
            "--renderer=gpu",
            f"--drm={scenario}",
            "--pattern=motion",
            "--frames=1",
            "--fps=0",
            *(["--verify"] if verify else []),
        ],
        name="report a GPU diagnostic failure without software fallback",
        timeout=5,
    )

    assert result.returncode == 1
    assert diagnostic in result.stderr
    assert "renderer=software" not in result.stderr
    assert result.stderr.endswith(f"fake-session restored presentations={presentations}\n")


def test_gpu_stop_drains_the_last_queued_frame_and_restores_the_session(
    display_test: DisplayTestBinaries,
) -> None:
    result = run_process(
        [
            str(display_test.command),
            "--renderer=gpu",
            "--drm=signal",
            "--pattern=motion",
            "--frames=3",
            "--fps=0",
        ],
        name="drain a queued GPU presentation after SIGTERM",
        timeout=5,
    )

    assert result.returncode == 143
    assert "summary completed=1 " in result.stdout
    assert result.stdout.endswith(" interrupted=yes\n")
    assert result.stderr.endswith(
        "fake-gpu restored draws=1 checks=0\nfake-session restored presentations=1\n"
    )


@pytest.mark.parametrize("scenario", ["gpu-second-draw-error", "gpu-second-verify-error"])
def test_gpu_later_failure_retains_the_preceding_completed_frame(
    display_test: DisplayTestBinaries, scenario: str
) -> None:
    result = run_process(
        [
            str(display_test.command),
            "--renderer=gpu",
            f"--drm={scenario}",
            "--verify",
            "--pattern=motion",
            "--frames=3",
            "--fps=0",
        ],
        name="retain a completed presentation when the next GPU frame fails",
        timeout=5,
    )

    assert result.returncode == 1
    lines = result.stdout.splitlines()
    assert len(lines) == 3
    assert lines[1].startswith("0,")
    assert lines[2].startswith("summary completed=1 ")
    assert result.stderr.endswith("fake-session restored presentations=1\n")


def test_one_minute_limit_is_accepted_without_forcing_a_minute_of_frames(
    display_test: DisplayTestBinaries,
) -> None:
    result = run_process(
        [
            str(display_test.command),
            "--pattern=motion",
            "--duration-ms=60000",
            "--frames=1",
            "--fps=0",
        ],
        name="accept a bounded one-minute visual run with an independent frame limit",
        timeout=5,
    )

    assert result.returncode == 0, result.stderr
    assert "summary completed=1 " in result.stdout

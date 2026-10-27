# SPDX-License-Identifier: GPL-2.0-only
"""Native GL video adapter with controlled context, renderer and display boundaries."""

from __future__ import annotations

import errno
import os
import shlex
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def quake_gpu_video(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the actual adapter and dispatch without a GL library or live devices."""
    with c_build_directory(tmp_path_factory, "quake-gpu-video") as directory:
        executable = directory / "quake-gpu-video"
        flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read DRM header flags for the Quake GPU adapter",
                timeout=10,
                check=True,
            ).stdout
        )
        compile_c(
            executable,
            [
                "-std=gnu99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-isystem",
                "/opt/quality/include",
                "-DNQ_HACK",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{ROOT / 'tests/host_tool/quake/quake-video-compat'}",
                f"-I{ROOT / 'alpine/aports/fplinux-tyrquake'}",
                str(ROOT / "tests/host_tool/quake/fplinux-quake-gpu-video.c"),
                str(ROOT / "alpine/aports/fplinux-tyrquake/vid_fplinux_gl.c"),
                str(ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake-gl.c"),
                "-Wl,--wrap=execv",
                *flags,
            ],
            name="compile Quake GPU video adapter with fake context and display",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario", ["portrait", "landscape", "small-portrait", "inactive", "engine-readback"]
)
def test_gpu_frames_preserve_engine_geometry_state_and_transfer_contents(
    quake_gpu_video: Path, scenario: str
) -> None:
    """The linked adapter copies pbuffer contents and delegates geometry to GPU output."""
    result = run_process(
        [str(quake_gpu_video), scenario],
        name="transfer a fake finished GPU frame through the native video adapter",
        timeout=5,
        check=True,
    )

    assert "Quake renderer: gpu; hardware-test; OpenGL 2.1 test; /dev/dri/renderD-test" in (
        result.stdout
    )
    assert "-> DMA-BUF -> DRM" in result.stdout
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout
    assert set(timing_values(result.stdout).values()) == {0}


def test_desktop_gl_dispatch_preserves_types_and_maps_only_pbuffer_color_buffers(
    quake_gpu_video: Path,
) -> None:
    """Double parameters and pointer returns use actual desktop GL declarations."""
    run_process(
        [str(quake_gpu_video), "dispatch"],
        name="exercise typed OpenGL dispatch and pbuffer buffer selection",
        timeout=5,
        check=True,
    )


@pytest.mark.parametrize(
    "scenario", ["gpu-fail", "missing-proc", "transfer-init-error", "init-gl-error"]
)
def test_auto_initialization_failure_releases_resources_before_software_handoff(
    quake_gpu_video: Path, scenario: str
) -> None:
    """Only startup failures may choose software, with unchanged public engine arguments."""
    result = run_process(
        [
            str(quake_gpu_video),
            scenario,
            "-heapsize",
            "16384",
            "+mlook",
            "-fplinux-software-fallback",
        ],
        name="observe GPU initialization unwind before software exec",
        timeout=5,
    )

    assert result.returncode == 37, result.stderr
    assert "GPU initialization failed:" in result.stderr
    assert "using software renderer\n" in result.stderr
    assert "software-fallback\n" in result.stdout
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout
    assert "Quake renderer: gpu;" not in result.stdout


@pytest.mark.parametrize(
    ("scenario", "error"),
    [
        ("gpu-fail", "no hardware OpenGL context"),
        ("missing-proc", "OpenGL entry point glMatrixMode is unavailable"),
        ("transfer-init-error", "OpenGL pbuffer setup failed:"),
        ("init-gl-error", "OpenGL initialization failed:"),
    ],
)
def test_explicit_gpu_initialization_failure_is_clear_and_never_uses_software(
    quake_gpu_video: Path, scenario: str, error: str
) -> None:
    """A required hardware renderer cannot silently change engines."""
    result = run_process(
        [str(quake_gpu_video), scenario],
        name="observe an explicit GPU startup error",
        timeout=5,
    )

    assert result.returncode == 1
    assert error in result.stderr
    assert "software-fallback" not in result.stdout
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout


@pytest.mark.parametrize(
    ("scenario", "error"),
    [
        ("runtime-gl-error", "rendering failed:"),
        ("runtime-copy-error", "frame copy failed:"),
        ("runtime-render-error", "output render: render failure"),
        ("runtime-present-error", "output present: present failure"),
        ("dispatch-fail", "VT dispatch:"),
    ],
)
def test_runtime_gpu_failure_stops_the_game_without_fallback(
    quake_gpu_video: Path, scenario: str, error: str
) -> None:
    """Auto mode must preserve a started session instead of restarting after an error."""
    result = run_process(
        [str(quake_gpu_video), scenario, "-fplinux-software-fallback"],
        name="observe GPU runtime failure without a second engine",
        timeout=5,
    )

    assert result.returncode == 1
    assert error in result.stderr
    assert "software-fallback" not in result.stdout
    assert "GPU initialization failed:" not in result.stderr
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout


@pytest.mark.parametrize("scenario", ["signal-init", "signal-runtime"])
def test_exit_signal_never_starts_a_software_session(quake_gpu_video: Path, scenario: str) -> None:
    """Cancellation applies during initialization and after the GPU has started."""
    result = run_process(
        [str(quake_gpu_video), scenario, "-fplinux-software-fallback"],
        name="stop native GPU video with SIGTERM",
        timeout=5,
        check=True,
    )

    assert "quit\n" in result.stdout
    assert "software-fallback" not in result.stdout
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout


@pytest.mark.parametrize(
    ("scenario", "error_number"),
    [
        ("drm-close-fail", errno.EIO),
        ("output-close-fail", errno.EIO),
        ("cleanup-both-fail", errno.EBUSY),
    ],
)
def test_failed_video_cleanup_blocks_software_fallback_and_preserves_first_error(
    quake_gpu_video: Path, scenario: str, error_number: int
) -> None:
    """The next engine cannot start when output or display cleanup reports failure."""
    result = run_process(
        [str(quake_gpu_video), scenario, "-fplinux-software-fallback"],
        name="refuse software handoff after failed video cleanup",
        timeout=5,
    )

    assert result.returncode == 1
    assert "cannot restore display:" in result.stderr
    assert os.strerror(error_number) in result.stderr
    assert "software-fallback" not in result.stdout


@pytest.mark.parametrize(
    ("scenario", "expected_status", "error_number"),
    [
        ("quit", 0, None),
        ("quit-output-close-fail", 1, errno.EIO),
        ("quit-drm-close-fail", 1, errno.EIO),
        ("quit-both-close-fail", 1, errno.EBUSY),
    ],
)
def test_normal_quit_reports_video_close_failure_after_host_cleanup(
    quake_gpu_video: Path, scenario: str, expected_status: int, error_number: int | None
) -> None:
    """A late output failure changes quit status without interrupting engine cleanup."""
    result = run_process(
        [str(quake_gpu_video), scenario, "-fplinux-software-fallback"],
        name="finish native video and host cleanup after normal quit",
        timeout=5,
    )

    assert result.returncode == expected_status, result.stderr
    assert "quit\n" in result.stdout
    assert "host-shutdown-complete: 1\n" in result.stdout
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout
    assert "software-fallback" not in result.stdout
    if error_number is not None:
        assert f"DRM close: {os.strerror(error_number)}\n" in result.stdout


def timing_values(stdout: str) -> dict[str, int]:
    line = next(line for line in stdout.splitlines() if line.startswith("gpu-timing: "))
    return {name: int(value) for name, value in (field.split("=") for field in line.split()[1:])}


def test_gpu_statistics_report_completed_scene_time_in_nanoseconds(quake_gpu_video: Path) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-completed"],
        name="report a scene timer after the existing GPU output completion",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["extensions"] == 1
    assert timing["counters"] == 1
    assert timing["queries"] == 1
    assert timing["begins"] == timing["ends"] == timing["results"] == 1
    assert timing["samples"] == timing["deletes"] == 1
    assert timing["ns"] == 18_000_000


@pytest.mark.parametrize(
    "scenario", ["timer-unsupported", "timer-missing-proc", "timer-no-counter"]
)
def test_unavailable_gpu_timer_preserves_rendering_without_samples(
    quake_gpu_video: Path, scenario: str
) -> None:
    result = run_process(
        [str(quake_gpu_video), scenario],
        name="render normally when optional scene timing is unavailable",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["extensions"] == 1
    assert timing["queries"] == timing["samples"] == timing["results"] == 0
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout


def test_gpu_timer_starts_after_enabled_and_stops_after_disabled(quake_gpu_video: Path) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-toggle"],
        name="enable scene timing after video initialization and disable it again",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["extensions"] == timing["counters"] == timing["queries"] == 1
    assert timing["begins"] == timing["ends"] == timing["results"] == timing["samples"] == 1
    assert timing["deletes"] == 1
    assert timing["ns"] == 18_000_000


def test_gpu_timer_probe_preserves_a_preexisting_rendering_error(quake_gpu_video: Path) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-preexisting-error", "-fplinux-software-fallback"],
        name="preserve an existing rendering error before optional timer probing",
        timeout=5,
    )

    assert result.returncode == 1
    assert "rendering failed:" in result.stderr
    assert timing_values(result.stdout)["queries"] == 0
    assert "software-fallback" not in result.stdout


def test_gpu_timer_begin_allocation_failure_preserves_rendering(quake_gpu_video: Path) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-begin-oom"],
        name="continue rendering when the driver cannot allocate an elapsed-time query",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["extensions"] == timing["counters"] == 1
    assert timing["queries"] == timing["begins"] == timing["deletes"] == 1
    assert timing["ends"] == timing["results"] == timing["samples"] == 0
    assert "host-shutdown-complete: 1\n" in result.stdout


def test_gpu_timer_preserves_scene_error_and_closes_active_query(quake_gpu_video: Path) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-scene-error", "-fplinux-software-fallback"],
        name="preserve a draw error while closing its active elapsed-time query",
        timeout=5,
    )

    assert result.returncode == 1
    assert "rendering failed: 0x502" in result.stderr
    timing = timing_values(result.stdout)
    assert timing["queries"] == timing["begins"] == timing["ends"] == timing["deletes"] == 1
    assert timing["results"] == timing["samples"] == 0
    assert "resources: output=0 gpu=0 display=0 input=0 signal_default=1\n" in result.stdout
    assert "software-fallback" not in result.stdout


def test_gpu_timer_survives_inactive_vt_until_output_completion(quake_gpu_video: Path) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-vt"],
        name="retain an ended scene timer while the VT is inactive and resume safely",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["queries"] == timing["deletes"] == 1
    assert timing["begins"] == timing["ends"] == timing["results"] == timing["samples"] == 2
    assert timing["ns"] == 18_000_000


@pytest.mark.parametrize("scenario", ["timer-close-active", "timer-close-pending"])
def test_gpu_timer_cleanup_balances_active_query_and_deletes_without_waiting(
    quake_gpu_video: Path, scenario: str
) -> None:
    result = run_process(
        [str(quake_gpu_video), scenario],
        name="close native video with an active or incomplete scene query",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["queries"] == timing["begins"] == timing["ends"] == timing["deletes"] == 1
    assert timing["results"] == timing["samples"] == 0
    assert "host-shutdown-complete: 1\n" in result.stdout


def test_failed_optional_gpu_timer_preserves_rendering_without_sample(
    quake_gpu_video: Path,
) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-query-error"],
        name="continue rendering when optional GPU timing cannot read its result",
        timeout=5,
        check=True,
    )

    timing = timing_values(result.stdout)
    assert timing["results"] == 1
    assert timing["samples"] == 0
    assert timing["queries"] == timing["deletes"] == 1
    assert "host-shutdown-complete: 1\n" in result.stdout


def test_gpu_timer_never_reports_an_incomplete_sample_after_output_failure(
    quake_gpu_video: Path,
) -> None:
    result = run_process(
        [str(quake_gpu_video), "timer-render-error", "-fplinux-software-fallback"],
        name="stop native GPU rendering without reading an incomplete timer",
        timeout=5,
    )

    assert result.returncode == 1
    assert "output render: render failure" in result.stderr
    timing = timing_values(result.stdout)
    assert timing["results"] == 0
    assert timing["samples"] == 0
    assert timing["queries"] == timing["deletes"] == 1
    assert "software-fallback" not in result.stdout

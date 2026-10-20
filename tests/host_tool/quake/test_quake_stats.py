# SPDX-License-Identifier: GPL-2.0-only
"""Native Quake HUD with controlled clocks and real fixture-backed Linux readings."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def quake_stats(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production HUD separately from controlled engine and Linux boundaries."""
    with c_build_directory(tmp_path_factory, "quake-stats") as directory:
        executable = directory / "quake-stats"
        compile_c(
            executable,
            [
                "-std=gnu99",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tests/host_tool/quake/quake-stats-compat'}",
                f"-I{ROOT / 'alpine/aports/fplinux-tyrquake'}",
                str(ROOT / "alpine/aports/fplinux-tyrquake/fplinux-quake-stats.c"),
                str(ROOT / "tests/host_tool/quake/fplinux-quake-stats.c"),
                "-Wl,--wrap=clock_gettime",
                "-Wl,--wrap=fopen",
                "-Wl,--wrap=glob",
                "-Wl,--wrap=sysconf",
            ],
            name="compile the Quake diagnostic HUD with controlled sample boundaries",
        )
        yield executable


@pytest.fixture
def readings(tmp_path: Path) -> Path:
    """Own the resident-page and thermal-zone files read through the real libc parser."""
    (tmp_path / "statm").write_text("8192 3072 0 0 0 0 0\n", encoding="ascii")
    thermal_zone = tmp_path / "thermal/thermal_zone0"
    thermal_zone.mkdir(parents=True)
    (thermal_zone / "temp").write_text("42500\n", encoding="ascii")
    return tmp_path


def test_frame_cpu_and_gpu_means_use_real_window_and_nanosecond_units(
    quake_stats: Path, readings: Path
) -> None:
    """An uneven 1.2-second window reports independent literal means and current RSS/TEMP."""
    run_process(
        [str(quake_stats), "metrics", str(readings)],
        name="observe Quake frame, process CPU, GPU scene and Linux resource metrics",
        timeout=5,
        check=True,
    )


def test_missing_and_invalid_rss_and_thermal_readings_recover(
    quake_stats: Path, readings: Path
) -> None:
    """Optional Linux readings may disappear and return without stopping the HUD."""
    (readings / "statm").unlink()
    (readings / "thermal/thermal_zone0/temp").unlink()
    run_process(
        [str(quake_stats), "optional-recovery", str(readings)],
        name="recover Quake optional metrics after missing and invalid readings",
        timeout=5,
        check=True,
    )


def test_multiple_thermal_zones_are_unavailable_and_page_size_failure_recovers(
    quake_stats: Path, readings: Path
) -> None:
    """Equal readings from distinct zones do not identify one temperature sensor."""
    second_zone = readings / "thermal/thermal_zone1"
    second_zone.mkdir()
    (second_zone / "temp").write_text("42500\n", encoding="ascii")
    run_process(
        [str(quake_stats), "ambiguous-temperature", str(readings)],
        name="observe unavailable ambiguous thermal zones and failed RSS page size",
        timeout=5,
        check=True,
    )


def test_disabled_hud_acquires_nothing_and_reenable_excludes_idle_and_previous_samples(
    quake_stats: Path, readings: Path
) -> None:
    """Opting out avoids clocks and file reads; opting in starts a fresh sampling window."""
    run_process(
        [str(quake_stats), "disabled", str(readings)],
        name="disable Quake telemetry acquisition and restart the sample window",
        timeout=5,
        check=True,
    )


def test_failed_process_and_monotonic_clocks_recover_without_stale_means(
    quake_stats: Path, readings: Path
) -> None:
    """A missing process baseline gives CPU unavailable until two valid boundaries exist."""
    run_process(
        [str(quake_stats), "clock-recovery", str(readings)],
        name="recover Quake telemetry after unavailable clocks",
        timeout=5,
        check=True,
    )


@pytest.mark.parametrize(
    ("width", "height"),
    [(320, 200), (240, 320), (320, 240), (128, 160)],
    ids=["minimum-render", "portrait", "landscape", "narrow"],
)
def test_panel_and_text_stay_inside_scaled_geometry_with_large_metrics(
    quake_stats: Path, readings: Path, width: int, height: int
) -> None:
    """The existing 8-pixel font fits normal render sizes and clips safely in narrow HUDs."""
    (readings / "statm").write_text("1000000 300000 0 0 0 0 0\n", encoding="ascii")
    (readings / "thermal/thermal_zone0/temp").write_text("-100000\n", encoding="ascii")
    run_process(
        [str(quake_stats), "geometry", str(readings), str(width), str(height)],
        name="draw a bounded Quake diagnostic panel in scaled HUD coordinates",
        timeout=5,
        check=True,
    )

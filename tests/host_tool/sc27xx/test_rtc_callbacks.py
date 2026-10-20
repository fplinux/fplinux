# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the patched RTC driver through registered callbacks and a PMIC regmap."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc27xx"


@pytest.fixture(scope="module")
def rtc_callbacks(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production driver separately from host subsystem substitutes."""
    with c_build_directory(tmp_path_factory, "sc27xx-rtc") as directory:
        rtc = directory / "drivers/rtc"
        rtc.mkdir(parents=True)
        shutil.copyfile(FIXTURES / "rtc-sc27xx.c.fixture", rtc / "rtc-sc27xx.c")
        run_process(
            [
                "patch",
                "--batch",
                "--forward",
                "--fuzz=0",
                "-p1",
                "-i",
                str(ROOT / "platforms/shared/linux/patches/0007-sc27xx-rtc.patch"),
            ],
            cwd=directory,
            name="apply the shared RTC patch to the pinned upstream fixtures",
            timeout=15,
            check=True,
        )
        executable = directory / "rtc-callbacks"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-Wno-sign-compare",
                "-Wno-shift-negative-value",
                "-I",
                str(FIXTURES / "rtc-compat"),
                str(FIXTURES / "rtc-callbacks.c"),
                str(rtc / "rtc-sc27xx.c"),
            ],
            name="link the real SC27xx RTC driver with host subsystem boundaries",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["sc2721-time", "sc2721-invalid", "sc2721-alarm", "sc2721-errors", "sc2720"],
    ids=[
        "sc2721-preserves-2019-epoch-and-calendar-encoding",
        "sc2721-stays-invalid-until-an-acknowledged-set-time",
        "sc2721-normal-alarm-is-one-shot-and-preserves-validity",
        "sc2721-register-errors-and-timeouts-remain-visible",
        "sc2720-preserves-1980-epoch-and-power-status-protocol",
    ],
)
def test_rtc_driver_callbacks(rtc_callbacks: Path, scenario: str) -> None:
    """Host evidence covers RTC callbacks, not real time, IRQ delivery or retention."""
    run_process(
        [str(rtc_callbacks), scenario],
        name=f"exercise linked SC27xx RTC callbacks: {scenario}",
        timeout=5,
        check=True,
    )

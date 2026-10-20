# SPDX-License-Identifier: GPL-2.0-only
"""Link the real regulator drivers against host regmap and registration boundaries."""

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
def regulator_callbacks(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Apply the shared patch to its pinned upstream fixture and link each driver separately."""
    with c_build_directory(tmp_path_factory, "sc27xx-regulators") as directory:
        regulator = directory / "drivers/regulator"
        regulator.mkdir(parents=True)
        for name in ("sc2731-regulator.c", "Kconfig", "Makefile"):
            shutil.copyfile(FIXTURES / f"{name}.fixture", regulator / name)
        run_process(
            [
                "patch",
                "--batch",
                "--forward",
                "--fuzz=0",
                "-p1",
                "-i",
                str(ROOT / "platforms/shared/linux/patches/0006-sc27xx-regulators.patch"),
            ],
            cwd=directory,
            name="apply the shared regulator patch to the pinned upstream driver",
            timeout=15,
            check=True,
        )
        core = ROOT / "platforms/shared/linux/overlay/drivers/regulator"
        sc2721 = ROOT / "platforms/sc9820e/linux/overlay/drivers/regulator"
        executable = directory / "regulator-callbacks"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-DCONFIG_REGULATOR_SC2720_USB33=1",
                "-DCONFIG_REGULATOR_SC2720_WCN=1",
                "-DCONFIG_REGULATOR_SC2720_WIFIPA=1",
                "-I",
                str(FIXTURES / "compat"),
                "-I",
                str(core),
                str(FIXTURES / "regulator-callbacks.c"),
                str(core / "sc27xx-regulator-core.c"),
                str(sc2721 / "sc2721-regulator.c"),
                str(regulator / "sc2731-regulator.c"),
            ],
            name="link the SC27xx core and both real regulator drivers with host boundaries",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    ["sc2721", "sc2721-connectivity", "sc2731", "sc2720-fixed", "probe-errors"],
    ids=[
        "sc2721-read-only-core-sd-voltage-and-default-probe",
        "sc2721-connectivity-voltage-range-enable-and-register-ownership",
        "sc2731-unlock-linear-voltage-and-async-probe",
        "sc2720-fixed-selectors-errors-and-sdio-delay",
        "probe-errors-preserve-errno-and-release-partial-registrations",
    ],
)
def test_regulator_driver_callbacks_preserve_variant_behavior(
    regulator_callbacks: Path, scenario: str
) -> None:
    """Host subsystem substitutes observe actual probes and descriptor callbacks."""
    run_process(
        [str(regulator_callbacks), scenario],
        name=f"exercise linked SC27xx regulator callbacks: {scenario}",
        timeout=5,
        check=True,
    )

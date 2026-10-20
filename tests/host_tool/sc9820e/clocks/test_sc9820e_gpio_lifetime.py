# SPDX-License-Identifier: GPL-2.0-only
"""Host GPIO probe and managed teardown with the real clock gate callbacks."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/clocks"
DRIVERS = ROOT / "platforms/sc9820e/linux/overlay/drivers/clk/sprd"
GPIO_PATCH = ROOT / "platforms/shared/linux/patches/0003-sprd-gpio.patch"


@pytest.fixture(scope="module")
def gpio_lifetime_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Apply the shared patch to a pinned original and link separate driver objects."""
    with c_build_directory(tmp_path_factory, "sc9820e_gpio_lifetime") as temporary:
        source = temporary / "drivers/gpio"
        source.mkdir(parents=True)
        for name in ("gpio-sprd.c", "Kconfig"):
            original = FIXTURES / "gpio-source" / f"{name}.fixture"
            (source / name).write_bytes(original.read_bytes())
        patch = run_process(
            ["patch", "--batch", "--fuzz=0", "-p1", "-i", str(GPIO_PATCH)],
            name="apply shared GPIO patch for host lifecycle component",
            timeout=10,
            cwd=temporary,
        )
        assert patch.returncode == 0, patch.stdout + patch.stderr
        executable = temporary / "sc9820e-gpio-lifetime"
        compilation = compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{FIXTURES / 'gpio-compat'}",
                f"-I{FIXTURES / 'gate-compat'}",
                str(FIXTURES / "sc9820e-gpio-lifetime.c"),
                str(source / "gpio-sprd.c"),
                str(DRIVERS / "sc9820e-gate.c"),
                str(DRIVERS / "sc9820e-peripheral-gates.c"),
            ],
            name="compile SC9820E GPIO managed-lifetime host component",
            check=False,
        )
        assert compilation.returncode == 0, compilation.stdout + compilation.stderr
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "probe-success",
        "map-failure",
        "gpio-failure",
        "enable-failure",
        "invalid-ngpio-zero",
        "invalid-ngpio-large",
        "optional-absent",
    ],
)
def test_gpio_probe_releases_controller_resources_before_clock(
    gpio_lifetime_component: Path, scenario: str
) -> None:
    """Kernel subsystem doubles release resources through actual GPIO/IRQ callbacks."""
    result = run_process(
        [str(gpio_lifetime_component), scenario],
        name=f"run SC9820E GPIO managed-lifetime host component: {scenario}",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr

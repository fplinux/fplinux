# SPDX-License-Identifier: GPL-2.0-only
"""Link production HP readiness and gain control to a bounded PMIC transport peer."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/hp"
SOURCE = ROOT / "targets/xiaomi-qin-1s-plus/linux/overlay/sound/drivers/qin1splus"


@pytest.fixture(scope="module")
def hp_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Replace the transport, clock-wait and IRQ boundary, retaining HP control flow."""
    with c_build_directory(tmp_path_factory, "qin-hp") as directory:
        executable = directory / "qin-hp-host"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(FIXTURES / "compat"),
                "-I",
                str(SOURCE),
                "-I",
                str(ROOT / "platforms/sc9820e/linux/overlay/include"),
                str(FIXTURES / "qin-hp-host.c"),
                str(SOURCE / "qin-hp.c"),
            ],
            name="compile Qin HP production component",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "muted-ready-stop",
        "foreign-output",
        "ledger-full",
        "mixed-controls",
        "calibration-timeout",
        "calibration-glitch",
        "depop-valid-timeout",
        "charge-timeout",
        "left-rdac-timeout",
        "right-rdac-timeout",
        "protection-at-prepare",
        "protection-irq",
        "irq-read-failure",
        "spurious-irq",
        "off-rdac-timeout",
        "cleanup-partial-failure",
        "partial-prepare-failures",
        "volume-before-ready",
        "volume-mapping",
        "volume-range",
        "volume-stale-protection",
        "volume-write-failure",
    ],
)
def test_hp_readiness_and_volume_retain_cleanup_and_protection_contracts(
    hp_component: Path, scenario: str
) -> None:
    """Check readiness, literal gains, status masks and independent quieting."""
    run_process(
        [str(hp_component), scenario],
        name=f"exercise HP readiness and volume: {scenario}",
        timeout=5,
        check=True,
    )

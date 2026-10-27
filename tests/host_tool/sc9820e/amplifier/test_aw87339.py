# SPDX-License-Identifier: GPL-2.0-only
"""Link the amplifier API and Qin profile with host GPIO/I2C/lifetime doubles."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

FIXTURES = ROOT / "tests/host_tool/sc9820e/amplifier"
DRIVERS = ROOT / "platforms/sc9820e/linux/overlay/drivers"
OVERLAY_INCLUDE = ROOT / "platforms/sc9820e/linux/overlay/include"
QIN_AUDIO = ROOT / "targets/xiaomi-qin-1s-plus/linux/overlay/sound/drivers/qin1splus"


@pytest.fixture(scope="module")
def amplifier_component(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Replace GPIO/I2C transport and registration, retaining production control flow."""
    with c_build_directory(tmp_path_factory, "aw87339") as directory:
        executable = directory / "aw87339-host"
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
                str(OVERLAY_INCLUDE),
                "-I",
                str(QIN_AUDIO),
                str(FIXTURES / "aw87339-host.c"),
                str(DRIVERS / "misc/aw87339.c"),
                str(QIN_AUDIO / "qin-hp.c"),
            ],
            name="compile the AW87339 host component",
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "identify-off-lifecycle",
        "unsupported-bus",
        "reset-unavailable",
        "cleanup-registration",
        "release-partial-failure",
        "wrong-id",
        "read-failure",
        "transient-id",
        "off-write-failure",
        "off-reset-failure",
    ],
)
def test_amplifier_identification_finishes_off_and_propagates_errors(
    amplifier_component: Path, scenario: str
) -> None:
    """Never enable output; retain an independent reset attempt on each partial failure."""
    run_process(
        [str(amplifier_component), scenario],
        name=f"exercise AW87339 host lifecycle: {scenario}",
        timeout=5,
        check=True,
    )


@pytest.mark.parametrize(
    "scenario",
    [
        "start-stop-profile",
        "start-assert-partial-failure",
        "start-release-partial-failure",
        "start-active-control-failure",
        "start-cleanup-write-failure",
        "start-cleanup-reset-failure",
        "stop-write-failure",
        "stop-reset-failure",
        "suspend-resume-stay-off",
        "resume-reset-failure",
    ],
)
def test_amplifier_start_and_stop_retain_safe_cleanup_obligations(
    amplifier_component: Path, scenario: str
) -> None:
    """Observe real profile transfers and reject activation while cleanup is owed."""
    run_process(
        [str(amplifier_component), scenario],
        name=f"exercise AW87339 host start/stop: {scenario}",
        timeout=5,
        check=True,
    )


@pytest.mark.parametrize("transfer", [0, 1, 9])
def test_amplifier_configuration_failure_never_reaches_active_control(
    amplifier_component: Path, transfer: int
) -> None:
    """Initial, first and last configuration failures still attempt OFF and reset."""
    run_process(
        [str(amplifier_component), "start-configuration-failure", str(transfer)],
        name=f"exercise AW87339 host configuration failure: {transfer}",
        timeout=5,
        check=True,
    )


@pytest.mark.parametrize(
    "scenario",
    [
        "get-missing-property",
        "get-missing-supplier",
        "get-unbound-supplier",
        "get-unready-supplier",
        "get-missing-clientdata",
        "get-wrong-driver",
        "get-link-failure",
        "get-action-failure",
        "get-managed-lifetime",
    ],
)
def test_amplifier_typed_get_balances_supplier_reference_and_consumer_link(
    amplifier_component: Path, scenario: str
) -> None:
    """The link double observes API use; it does not model kernel unbind/PM order."""
    run_process(
        [str(amplifier_component), scenario],
        name=f"exercise AW87339 host supplier lifetime: {scenario}",
        timeout=5,
        check=True,
    )


def test_i2c2_clock_reports_only_undivided_oscillator_and_isolates_gate(
    tmp_path_factory: pytest.TempPathFactory,
) -> None:
    """Observe source changes without writes and gate only the I2C2 AP-APB bit."""
    with c_build_directory(tmp_path_factory, "i2c2-clock") as directory:
        executable = directory / "i2c2-clock-host"
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(FIXTURES / "compat"),
                str(FIXTURES / "i2c2-clock-host.c"),
                str(DRIVERS / "clk/sprd/sc9820e-i2c2-clock.c"),
                str(DRIVERS / "clk/sprd/sc9820e-gate.c"),
            ],
            name="compile I2C2 clock and gate host component",
        )
        run_process(
            [str(executable)],
            name="exercise I2C2 inherited clock and gate",
            timeout=5,
            check=True,
        )

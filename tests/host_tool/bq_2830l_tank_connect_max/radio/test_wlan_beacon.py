# SPDX-License-Identifier: GPL-2.0-only
"""Host checks of Marlin2 cfg80211 beacon callbacks with a fake transport."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/overlay/drivers/mmc/host/bq2830l-radio"
FIXTURES = ROOT / "tests/host_tool/bq_2830l_tank_connect_max/radio"
KERNEL_COMPAT = ROOT / "tests/fixtures/ums9117-kernel"


@pytest.fixture(scope="module")
def wlan_beacon(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Compile real callbacks separately; cfg80211, netdev and transport are fakes."""
    with c_build_directory(tmp_path_factory, "marlin2-wlan-beacon") as temporary:
        executable = temporary / "wlan-beacon"
        compile_c(
            executable,
            [
                "-O2",
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-UNDEBUG",
                "-pthread",
                f"-I{FIXTURES / 'wlan-beacon-compat'}",
                f"-I{FIXTURES / 'marlin2-owner-compat'}",
                f"-I{KERNEL_COMPAT / 'fm-compat'}",
                f"-I{KERNEL_COMPAT / 'bluetooth-compat'}",
                f"-I{KERNEL}",
                str(FIXTURES / "wlan-beacon.c"),
                str(KERNEL / "marlin2-wlan.c"),
                str(KERNEL / "marlin2-wlan-wire.c"),
                str(KERNEL / "marlin2-transport-wire.c"),
            ],
            name="compile Marlin2 beacon callback host checks",
            check=True,
        )
        yield executable


def run_beacon_case(wlan_beacon: Path, scenario: str) -> None:
    result = run_process(
        [str(wlan_beacon), scenario],
        name="run Marlin2 beacon callback host check",
        timeout=10,
        check=True,
    )
    assert result.stdout == "Marlin2 beacon callback host case passed\n"


@pytest.mark.parametrize("scenario", ["head", "tail", "empty-tail", "mixed"])
def test_replacement_is_rejected_without_firmware_commands(
    wlan_beacon: Path, scenario: str
) -> None:
    """Main-beacon replacement must fail before even supported extra IE writes."""
    run_beacon_case(wlan_beacon, scenario)


def test_extra_ie_callback_update_sends_three_ie_commands(wlan_beacon: Path) -> None:
    """Exercise driver callbacks; this does not execute nl80211 or a phone."""
    run_beacon_case(wlan_beacon, "extra-ies")


def test_beacon_change_before_ap_start_returns_enetdown(wlan_beacon: Path) -> None:
    run_beacon_case(wlan_beacon, "not-started")

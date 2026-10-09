# SPDX-License-Identifier: GPL-2.0-only
"""Host-linked Bluetooth, FM and shared packet wire contracts."""

from __future__ import annotations

from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

KERNEL = ROOT / "targets/bq-2830l-tank-connect-max/linux/drivers/mmc/host"
FIXTURES = ROOT / "tests/host_tool/ums9117"


def run_wire_component(tmp_path: Path, *, component: str, expected_stdout: str) -> None:
    executable = tmp_path / f"marlin2-{component}-wire"
    compile_c(
        executable,
        [
            "-O2",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-UNDEBUG",
            f"-I{FIXTURES / 'marlin2-packet-compat'}",
            f"-I{FIXTURES / 'fm-compat'}",
            f"-I{FIXTURES / 'bluetooth-compat'}",
            f"-I{KERNEL}",
            str(FIXTURES / f"marlin2-{component}-wire.c"),
            str(KERNEL / f"marlin2-{component}-wire.c"),
        ],
        name=f"compile Marlin2 {component} wire host checks",
        check=True,
    )
    result = run_process(
        [str(executable)],
        name=f"run Marlin2 {component} wire host checks",
        timeout=10,
        check=True,
    )
    assert result.stdout == expected_stdout


def test_h4_transmit_lengths_padding_and_setup_reply_matching(tmp_path: Path) -> None:
    """Link H4 wire code; HCI core and the controller do not execute."""
    run_wire_component(
        tmp_path,
        component="bluetooth",
        expected_stdout="Marlin2 Bluetooth wire host cases passed\n",
    )


def test_fm_commands_split_replies_seek_ack_and_failed_transactions(tmp_path: Path) -> None:
    """Link command and event code; V4L2 core and the receiver do not execute."""
    run_wire_component(
        tmp_path,
        component="fm",
        expected_stdout="Marlin2 FM wire host cases passed\n",
    )


def test_packet_records_mixed_aggregates_dummy_accounting_and_limits(tmp_path: Path) -> None:
    """Link shared framing code; IRQ, SDIO and consumer lifetimes do not execute."""
    run_wire_component(
        tmp_path,
        component="transport",
        expected_stdout="Marlin2 shared wire host cases passed\n",
    )

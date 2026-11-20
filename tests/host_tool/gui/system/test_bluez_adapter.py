# SPDX-License-Identifier: GPL-2.0-only
"""The shared BlueZ adapter client against a test-owned BlueZ on a private bus.

The fake replaces bluetoothd at its D-Bus boundary; no controller is involved.
"""

from __future__ import annotations

import subprocess
import time
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.gui.support.private_bus import BLUEZ_FAKE, dbus_flags, private_bus
from tests.process import read_process_line, run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HERE = ROOT / "tests/host_tool/gui/system"


@pytest.fixture(scope="module")
def adapter_test(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production client with the fake in one test process."""
    cflags, libraries = dbus_flags()
    with c_build_directory(tmp_path_factory, "bluez-adapter") as directory:
        executable = directory / "bluez-adapter-test"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                "-I",
                str(ROOT / "tests/host_tool/gui/support"),
                *cflags,
                str(HERE / "bluez-adapter-test.c"),
                str(BLUEZ_FAKE),
                str(ROOT / "lib/fplinux-shared/fplinux-bluez-adapter.c"),
                *libraries,
            ],
            name="compile the BlueZ adapter client and its fake",
        )
        yield executable


def test_absent_bus_leaves_bluetooth_absent_without_busy_wakeups(
    adapter_test: Path, tmp_path: Path
) -> None:
    result = run_process(
        [str(adapter_test), "no-bus"],
        name="BlueZ adapter without a bus",
        timeout=10,
        env={"DBUS_SYSTEM_BUS_ADDRESS": f"unix:path={tmp_path / 'absent-bus'}"},
        check=True,
    )
    assert result.stdout == "bluez-adapter: no bus leaves Bluetooth absent and idle\n"


def test_one_adapter_switches_and_follows_bluez_restarts(adapter_test: Path) -> None:
    """Live changes, refused and unanswered requests, two adapters and a restart."""
    with private_bus() as bus:
        result = run_process(
            [str(adapter_test), "lifecycle"],
            name="BlueZ adapter lifecycle",
            timeout=30,
            env=bus.environment(),
            check=True,
        )
    assert result.stdout == (
        "bluez-adapter: one adapter switches, follows other clients and bluetoothd restarts\n"
    )


@pytest.mark.parametrize(
    ("scenario", "expected"),
    [
        (
            "settings-inventory",
            "bluez-adapter: real inventory properties and owned discovery sessions\n",
        ),
        (
            "pairing-agent",
            "bluez-adapter: authenticated pairing requests require real user replies\n",
        ),
    ],
)
def test_settings_bluez_protocol_behaviors(
    adapter_test: Path, scenario: str, expected: str
) -> None:
    with private_bus() as bus:
        result = run_process(
            [str(adapter_test), scenario],
            name=f"BlueZ settings {scenario}",
            timeout=30,
            env=bus.environment(),
            check=True,
        )
    assert result.stdout == expected


def test_lost_bus_leaves_bluetooth_absent_without_busy_wakeups(adapter_test: Path) -> None:
    with private_bus() as bus:
        deadline = time.monotonic() + 20
        process = subprocess.Popen(
            [str(adapter_test), "bus-loss"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=bus.environment(),
        )
        try:
            assert process.stdout is not None
            assert process.stdin is not None
            connected = read_process_line(process.stdout, deadline=deadline, name="BlueZ adapter")
            assert connected == "bluez-adapter: connected\n"
            bus.stop()
            stdout, stderr = process.communicate(
                "go\n", timeout=max(0, deadline - time.monotonic())
            )
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
    assert process.returncode == 0, stderr
    assert stdout == "bluez-adapter: a lost bus leaves Bluetooth absent and idle\n"

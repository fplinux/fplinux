# SPDX-License-Identifier: GPL-2.0-only
"""The shared BlueZ adapter client against a test-owned BlueZ on a private bus.

The fake replaces bluetoothd at its D-Bus boundary; no controller is involved.
"""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.gui.private_bus import BLUEZ_FAKE, dbus_flags, private_bus
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HERE = ROOT / "tests/host_tool/gui"


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
                *cflags,
                str(HERE / "bluez-adapter-test.c"),
                str(BLUEZ_FAKE),
                str(ROOT / "lib/fplinux/fplinux-bluez-adapter.c"),
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


def test_lost_bus_leaves_bluetooth_absent_without_busy_wakeups(adapter_test: Path) -> None:
    with private_bus() as bus:
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
            assert process.stdout.readline() == "bluez-adapter: connected\n"
            bus.stop()
            stdout, stderr = process.communicate("go\n", timeout=20)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()
    assert process.returncode == 0, stderr
    assert stdout == "bluez-adapter: a lost bus leaves Bluetooth absent and idle\n"

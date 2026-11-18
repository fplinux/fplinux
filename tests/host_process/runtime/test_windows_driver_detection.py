# SPDX-License-Identifier: GPL-2.0-only
"""Exercise loader and probe processes with controlled Windows discovery responses."""

from __future__ import annotations

import subprocess
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime.windows import boot
from fplinux_cli.runtime.windows import host as windows_host

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.windows_host_support import StubWindowsHost

if TYPE_CHECKING:
    from collections.abc import Callable


@pytest.mark.parametrize(
    ("devices", "missing"),
    [
        ([{"id": "USB\\VID_1782&PID_4D00\\phone", "problem_code": 28}], True),
        ([{"id": "USB\\VID_1782&PID_4D01\\phone", "problem_code": 28}], False),
        ([{"id": "USB\\VID_1782&PID_4D00&MI_00\\phone", "problem_code": 28}], False),
        ([{"id": "USB\\VID_1782&PID_4D00\\phone", "problem_code": 0}], False),
        ([{"id": "USB\\VID_1782&PID_4D00\\phone", "problem_code": 10}], False),
        ([{"id": "USB\\VID_1782&PID_4D00\\phone"}], False),
    ],
    ids=[
        "missing-driver",
        "other-device",
        "interface",
        "working-driver",
        "other-problem",
        "unknown",
    ],
)
def test_waiting_loader_stops_only_for_matching_missing_driver(
    tmp_path: Path, devices: list[dict[str, Any]], *, missing: bool
) -> None:
    """The loader is a real child; Windows device discovery is a controlled response."""
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    observed = threading.Event()

    class DeviceHost(StubWindowsHost):
        def ps(
            self,
            script: str,
            *arguments: str,
            timeout: float = 20,
            cancelled: Callable[[], bool] | None = None,
        ) -> dict[str, Any]:
            del timeout
            assert (script, arguments) == (
                "devices.ps1",
                ("-DriverInfo", "-VendorId", "6018", "-ProductId", "19712"),
            )
            assert cancelled is not None
            deadline = time.monotonic() + 3
            while not (tmp_path / "usb-opened").exists():
                assert not cancelled()
                assert time.monotonic() < deadline
                time.sleep(0.01)
            observed.set()
            return {"devices": devices}

    host = DeviceHost({})
    acquired: list[str] = []

    def release() -> None:
        if observed.is_set() and not missing:
            (tmp_path / "release-ready").write_text("continue\n")

    def run() -> None:
        with boot._watch_bootrom_driver(  # noqa: SLF001 -- loader process boundary.
            host,
            {"vendor_id": 0x1782, "product_id": 0x4D00},
            on_usb_ready=lambda: acquired.append("ready"),
            on_wait=release,
        ) as (ready, waiting):
            host.run_tool(
                [str(loader), "waiting", str(tmp_path)],
                timeout=5,
                cwd=tmp_path,
                log=tmp_path / "runtime.log",
                on_usb_ready=ready,
                on_wait=waiting,
            )

    if missing:
        with pytest.raises(windows_host.BootromDriverRequiredError):
            run()
        assert acquired == []
        assert not (tmp_path / "usb-transfer-started").exists()
    else:
        run()
        assert acquired == ["ready"]
        assert (tmp_path / "usb-transfer-started").read_text() == "acknowledged\n"
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


def test_timed_out_driver_probe_does_not_stop_loader(tmp_path: Path) -> None:
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    retried = threading.Event()

    class TimedOutHost(StubWindowsHost):
        attempts = 0

        def ps(
            self,
            script: str,
            *arguments: str,
            timeout: float = 20,
            cancelled: Callable[[], bool] | None = None,
        ) -> dict[str, Any]:
            del script, arguments, cancelled
            self.attempts += 1
            if self.attempts == 1:
                command = "PnP"
                raise subprocess.TimeoutExpired(command, timeout)
            retried.set()
            return {"devices": []}

    host = TimedOutHost({})

    def release() -> None:
        if retried.is_set():
            (tmp_path / "release-ready").write_text("continue\n")

    with boot._watch_bootrom_driver(  # noqa: SLF001 -- loader process boundary.
        host,
        {"vendor_id": 0x1782, "product_id": 0x4D00},
        on_usb_ready=lambda: None,
        on_wait=release,
    ) as (ready, waiting):
        host.run_tool(
            [str(loader), "waiting", str(tmp_path)],
            timeout=5,
            cwd=tmp_path,
            log=tmp_path / "runtime.log",
            on_usb_ready=ready,
            on_wait=waiting,
        )

    assert (tmp_path / "usb-transfer-started").read_text() == "acknowledged\n"
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


def test_slow_missing_driver_query_reaches_setup_before_native_transfer(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A real six-second query must deliver Code 28 while native discovery waits."""
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_discovery_slow.py",
        powershell,
    )
    probe_pid = tmp_path / "probe.pid"
    monkeypatch.setenv("FPLINUX_TEST_DISCOVERY_PID", str(probe_pid))
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = powershell

    with (
        pytest.raises(windows_host.BootromDriverRequiredError),
        boot._watch_bootrom_driver(  # noqa: SLF001 -- loader process boundary.
            host,
            {"vendor_id": 0x1782, "product_id": 0x4D00},
            on_usb_ready=lambda: pytest.fail("driverless USB must not start transfer"),
            on_wait=None,
        ) as (ready, waiting),
    ):
        host.run_tool(
            [str(loader), "waiting", str(tmp_path)],
            timeout=12,
            cwd=tmp_path,
            log=tmp_path / "runtime.log",
            on_usb_ready=ready,
            on_wait=waiting,
        )

    assert not (tmp_path / "usb-transfer-started").exists()
    assert not (Path("/proc") / probe_pid.read_text()).exists()
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()


@pytest.mark.parametrize("outcome", ["usb-ready", "wait-error"])
def test_loader_completion_or_error_cancels_owned_discovery_process(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, outcome: str
) -> None:
    """The PowerShell stand-in sleeps until production cancellation reaps it."""
    loader = tmp_path / "loader.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/usb_loader.py", loader
    )
    powershell = tmp_path / "powershell.exe"
    install_python_script(
        ROOT / "tests/host_process/runtime/fixtures/windows-host/windows_discovery_timeout.py",
        powershell,
    )
    probe_pid = tmp_path / "probe.pid"
    monkeypatch.setenv("FPLINUX_TEST_DISCOVERY_PID", str(probe_pid))
    host = windows_host.WindowsHost.__new__(windows_host.WindowsHost)
    host.powershell = powershell

    def release_or_fail() -> None:
        if probe_pid.exists():
            if outcome == "wait-error":
                message = "stop waiting"
                raise ValueError(message)
            (tmp_path / "release-ready").write_text("continue\n")

    def run() -> None:
        with boot._watch_bootrom_driver(  # noqa: SLF001 -- loader process boundary.
            host,
            {"vendor_id": 0x1782, "product_id": 0x4D00},
            on_usb_ready=lambda: None,
            on_wait=release_or_fail,
        ) as (ready, waiting):
            host.run_tool(
                [str(loader), "waiting", str(tmp_path)],
                timeout=5,
                cwd=tmp_path,
                log=tmp_path / "runtime.log",
                on_usb_ready=ready,
                on_wait=waiting,
            )

    if outcome == "wait-error":
        with pytest.raises(ValueError, match="stop waiting"):
            run()
        assert not (tmp_path / "usb-transfer-started").exists()
    else:
        run()
        assert (tmp_path / "usb-transfer-started").read_text() == "acknowledged\n"
    assert not (Path("/proc") / probe_pid.read_text()).exists()
    assert not (Path("/proc") / (tmp_path / "child.pid").read_text()).exists()

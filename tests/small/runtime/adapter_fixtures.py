# SPDX-License-Identifier: GPL-2.0-only
"""Test-owned adapter inputs and controlled USB, clock and bridge boundaries."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

from tests import ROOT

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import ModuleType


def load_adapter() -> ModuleType:
    """Load the shipped standalone adapter without requiring a Python package."""
    path = ROOT / "platforms/ums9117/host/adapter.py"
    spec = importlib.util.spec_from_file_location("ums9117_host_adapter", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"unable to load adapter: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ADAPTER = load_adapter()


class FakeClock:
    """Monotonic time that passes only while the code under test sleeps."""

    def __init__(self) -> None:
        """Start at zero seconds."""
        self.now = 0.0

    def monotonic(self) -> float:
        """Return the current time for any number of reads."""
        return self.now

    def sleep(self, seconds: float) -> None:
        """Advance the time by the requested wait without blocking."""
        self.now += seconds


def adapter_data(**overrides: object) -> dict[str, object]:
    """Return one valid adapter table with optional focused overrides."""
    data: dict[str, object] = {
        "brightness": 50,
        "rotation": 0,
        "spi_mode": 1,
        "lcd_id": 0x8888B6,
        "exec_distance": 0x314D,
        "backlight_channels": "rgbw",
        "backlight_level": 0x14,
        "session_name": "test-linux",
        "handoff_wait_seconds": 60,
        "usb_release_wait_seconds": 10,
        "boot_instructions": "Hold the boot key and connect USB.",
    }
    data.update(overrides)
    return data


def headless_adapter_data() -> dict[str, object]:
    """Return one valid adapter table without the loader display settings."""
    data = adapter_data()
    for key in ("spi_mode", "lcd_id", "backlight_channels", "backlight_level"):
        del data[key]
    return data


def runtime_identity(*, platform_name: str = "ums9117") -> dict[str, object]:
    """Return the identity portion consumed by the fixed UMS9117 adapter."""
    return {
        "identity": {
            "target": {"display_name": "Nokia 3210 4G (TA-1618)"},
            "platform": {"name": platform_name},
        }
    }


class BridgeProcess:
    """Minimal bridge process whose exit code is the protocol result."""

    def __init__(self, status: int | None, *, time_out: bool = False) -> None:
        """Set one exit outcome, optionally preceded by an acknowledgement timeout."""
        self.status = status
        self.time_out = time_out
        self.returncode: int | None = None
        self.wait_timeouts: list[int] = []
        self.terminated = False
        self.killed = False

    def poll(self) -> int | None:
        """Expose the process state used by the adapter cleanup path."""
        return self.returncode

    def wait(self, timeout: int | None = None) -> int:
        """Report one configured bridge outcome, then allow deterministic cleanup."""
        if timeout is not None:
            self.wait_timeouts.append(timeout)
        if self.time_out:
            self.time_out = False
            command = "libc_server"
            wait_timeout = timeout if timeout is not None else 0
            raise subprocess.TimeoutExpired(command, wait_timeout)
        if self.returncode is None:
            self.returncode = self.status
        if self.returncode is None:
            message = "bridge cleanup did not choose an exit status"
            raise AssertionError(message)
        return self.returncode

    def terminate(self) -> None:
        """Model normal process termination after an acknowledgement timeout."""
        self.terminated = True
        self.status = -15

    def kill(self) -> None:
        """Model forced process termination if normal termination did not complete."""
        self.killed = True
        self.status = -9


class BridgeFixture(unittest.TestCase):
    """Prepare adapter inputs and control the bridge-process boundary without a phone."""

    session_id = "a" * 64

    def runtime(
        self,
        transport: str = "none",
    ) -> dict[str, Any]:
        """Return the complete adapter input for one bridge acknowledgement case."""
        return {
            **runtime_identity(),
            "transport": transport,
            "assets": {
                "fdl1": "assets/fdl1.bin",
                "pinmap": "assets/pinmap.bin",
                "keymap": "assets/keymap.bin",
            },
            "host_tools": {
                "loader": "host/spd_dump",
                "bridge": "host/libc_server",
                "keyboard": "host/fplinux-usb-keyboard",
            },
            "adapter": adapter_data(),
            "image": "image/ramboot.bin",
            "addresses": {"fdl1": 0x6200, "payload": 0x80100000},
            "usb": {
                "bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 1},
                "linux_gadget": {
                    "vendor_id": 0x0525,
                    "product_id": 0xA4A6,
                    "wait_seconds": 30,
                },
            },
        }

    def session(self) -> dict[str, str]:
        """Return the prepared session selected by the current runner invocation."""
        return {"session_id": self.session_id, "image": str(ROOT / "ramboot.bin")}

    def run_bridge(  # noqa: PLR0913 -- each keyword replaces one external boundary.
        self,
        bridge: BridgeProcess,
        *,
        transport: str = "none",
        runtime: dict[str, Any] | None = None,
        start_bridge: Callable[..., BridgeProcess] | None = None,
        output: io.StringIO | None = None,
        bootrom_release_seconds: float | None = 0.0,
        events: Callable[[str], None] | None = None,
    ) -> tuple[mock.Mock, mock.Mock, Path]:
        """Run the adapter through its bridge-process boundary with no phone attached.

        The BootROM node disappears once the adapter has waited
        ``bootrom_release_seconds``; ``None`` keeps it present.
        """
        clock = FakeClock()

        def bootrom_present() -> bool:
            return bootrom_release_seconds is None or clock.now < bootrom_release_seconds

        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            bootrom = mock.Mock(spec=Path)
            bootrom.exists.side_effect = bootrom_present
            transport_module = mock.Mock()
            popen = (
                mock.Mock(return_value=bridge)
                if start_bridge is None
                else mock.Mock(side_effect=start_bridge)
            )
            patches: list[contextlib.AbstractContextManager[object]] = [
                mock.patch.object(ADAPTER, "usb_device_path", return_value=bootrom),
                mock.patch.object(ADAPTER, "require_usb_device_access"),
                mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
                mock.patch.object(ADAPTER.time, "sleep", clock.sleep),
                mock.patch.object(
                    ADAPTER.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0),
                ),
                mock.patch.object(ADAPTER.subprocess, "Popen", popen),
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
            ]
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(output or io.StringIO()))
                for patch in patches:
                    stack.enter_context(patch)
                ADAPTER.run(
                    bundle,
                    runtime or self.runtime(transport),
                    self.session(),
                    expected_device_identity="9" * 64,
                    events=events,
                )
        return popen, transport_module, bundle

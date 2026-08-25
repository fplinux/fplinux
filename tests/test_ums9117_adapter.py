# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the data-driven UMS9117 RAM adapter."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import unittest
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[1]


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


class UsbDeviceAccessTests(unittest.TestCase):
    """Report disappearance and permission denial as different USB failures."""

    device = Path("/dev/bus/usb/007/053")

    def test_vanished_node_reports_disconnect(self) -> None:
        """A transient USB disappearance must not be diagnosed as a udev failure."""
        with (
            mock.patch.object(ADAPTER.os, "open", side_effect=FileNotFoundError),
            self.assertRaisesRegex(SystemExit, "disconnected before the RAM loader"),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

    def test_permission_denial_reports_access_control(self) -> None:
        """An existing node rejected by the OS keeps the actionable udev diagnosis."""
        with (
            mock.patch.object(ADAPTER.os, "open", side_effect=PermissionError),
            self.assertRaisesRegex(SystemExit, "not readable and writable"),
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

    def test_successful_probe_closes_the_usbfs_node(self) -> None:
        """The access probe must release its descriptor before libc opens the device."""
        with (
            mock.patch.object(ADAPTER.os, "open", return_value=41) as open_device,
            mock.patch.object(ADAPTER.os, "close") as close_device,
        ):
            ADAPTER.require_usb_device_access(self.device, "1782:4d00")

        open_device.assert_called_once_with(
            self.device,
            ADAPTER.os.O_RDWR | ADAPTER.os.O_CLOEXEC,
        )
        close_device.assert_called_once_with(41)


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
        "handoff_marker": "TEST PREPARE LINUX",
        "handoff_wait_seconds": 60,
        "release_wait_seconds": 10,
        "boot_instructions": "Hold the boot key and connect USB.",
    }
    data.update(overrides)
    return data


class LoaderArgumentsTests(unittest.TestCase):
    """Keep exec-distance selection entirely target-driven."""

    def test_nonzero_exec_distance_precedes_ram_loads(self) -> None:
        """Emit the optional setup command before both RAM FDL commands."""
        arguments = ADAPTER.loader_arguments(
            Path("loader"),
            Path("fdl1.bin"),
            Path("image.bin"),
            {"fdl1": 0x6200, "payload": 0x80100000},
            0x314D,
        )
        self.assertEqual(
            arguments,
            [
                "loader",
                "t117_exec_dist",
                "0x314d",
                "fdl",
                "fdl1.bin",
                "0x6200",
                "fdl",
                "image.bin",
                "0x80100000",
            ],
        )

    def test_zero_exec_distance_omits_setup_command(self) -> None:
        """Start directly with the RAM FDL load when the target requests zero."""
        arguments = ADAPTER.loader_arguments(
            Path("loader"),
            Path("fdl1.bin"),
            Path("image.bin"),
            {"fdl1": 0x6200, "payload": 0x80100000},
            0,
        )
        self.assertEqual(
            arguments,
            [
                "loader",
                "fdl",
                "fdl1.bin",
                "0x6200",
                "fdl",
                "image.bin",
                "0x80100000",
            ],
        )


class BacklightConfigurationTests(unittest.TestCase):
    """Reject ambiguous channel spelling and out-of-range levels."""

    def test_canonical_channel_subset_formats_bl_extra(self) -> None:
        """Preserve the selected RGB subset in the bridge argument."""
        config = ADAPTER.adapter_config(
            adapter_data(backlight_channels="rgb", backlight_level=0x1F)
        )
        self.assertEqual(ADAPTER.backlight_argument(config), "rgb=0x1f")

    def test_noncanonical_or_invalid_channels_are_rejected(self) -> None:
        """Accept only non-empty channels in canonical rgbw order."""
        for channels in ("", "gr", "rr", "rgbwx"):
            with (
                self.subTest(channels=channels),
                self.assertRaisesRegex(
                    SystemExit,
                    "backlight_channels",
                ),
            ):
                ADAPTER.adapter_config(adapter_data(backlight_channels=channels))

    def test_backlight_level_above_encoded_range_is_rejected(self) -> None:
        """Keep the level within the six-bit libc_server field."""
        with self.assertRaisesRegex(SystemExit, "backlight_level"):
            ADAPTER.adapter_config(adapter_data(backlight_level=0x40))


class HandoffTransportTests(unittest.TestCase):
    """Keep a host-only handoff independent of USB-NCM acquisition."""

    def test_none_transport_returns_without_acquiring_ssh(self) -> None:
        """A no-transport profile completes after handoff without acquiring SSH/NCM."""
        rendered = io.StringIO()
        with (
            contextlib.redirect_stdout(rendered),
            mock.patch.object(
                ADAPTER.importlib,
                "import_module",
                side_effect=AssertionError("none transport must not acquire SSH/NCM"),
            ),
        ):
            ADAPTER.complete_linux_handoff(
                {"transport": "none"},
                None,
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 30},
            )

        self.assertEqual(
            rendered.getvalue(),
            "Bootstrap handoff marker observed; no host-side transport is available "
            "to confirm Linux startup.\n",
        )


if __name__ == "__main__":
    unittest.main()

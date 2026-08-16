# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the data-driven UMS9117 RAM adapter."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

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

    def test_backlight_level_above_hardware_range_is_rejected(self) -> None:
        """Keep the level within the six-bit libc_server field."""
        with self.assertRaisesRegex(SystemExit, "backlight_level"):
            ADAPTER.adapter_config(adapter_data(backlight_level=0x40))


if __name__ == "__main__":
    unittest.main()

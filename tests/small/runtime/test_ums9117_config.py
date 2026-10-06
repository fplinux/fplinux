# SPDX-License-Identifier: GPL-2.0-only
"""UMS9117 adapter config behavior."""

from __future__ import annotations

import io
import unittest
from pathlib import Path

from tests.small.runtime.adapter_fixtures import (
    ADAPTER,
    BridgeFixture,
    BridgeProcess,
    adapter_data,
    headless_adapter_data,
    runtime_identity,
)


class RuntimeIdentityTests(unittest.TestCase):
    """Keep platform selection and the rendered target identity fail-closed."""

    def test_reads_the_nested_target_display_name(self) -> None:
        """The adapter renders the validated target identity rather than a root alias."""
        runtime = runtime_identity()
        runtime["display_name"] = "Root Alias Phone"

        self.assertEqual(
            ADAPTER.runtime_target_display_name(runtime),
            "Nokia 3210 4G (TA-1618)",
        )

    def test_rejects_a_bundle_for_another_platform(self) -> None:
        """The fixed adapter refuses a runtime manifest for another platform."""
        with self.assertRaisesRegex(SystemExit, "platform identity must name ums9117"):
            ADAPTER.runtime_target_display_name(runtime_identity(platform_name="other"))

    def test_rejects_runtime_without_identity_object(self) -> None:
        """Root display_name and platform keys without an identity object are refused."""
        with self.assertRaisesRegex(SystemExit, "runtime identity must be an object"):
            ADAPTER.runtime_target_display_name(
                {"display_name": "Demo Phone", "platform": "ums9117"}
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


class DisplaySettingsTests(unittest.TestCase):
    """Accept the loader display settings as one complete set or not at all."""

    def test_incomplete_display_settings_are_rejected(self) -> None:
        """A partial LCD description is neither a display nor a headless loader."""
        for missing in ("spi_mode", "lcd_id", "backlight_channels", "backlight_level"):
            data = adapter_data()
            del data[missing]
            with (
                self.subTest(missing=missing),
                self.assertRaisesRegex(SystemExit, "adapter data must contain exactly"),
            ):
                ADAPTER.adapter_config(data)

        with self.assertRaisesRegex(SystemExit, "adapter data must contain exactly"):
            ADAPTER.adapter_config({**headless_adapter_data(), "lcd_id": 0x8888B6})


class AdapterPreflightTests(BridgeFixture):
    """The loader result, bridge exit status and BootROM release gate the Linux transition."""

    def test_invalid_preflight_never_invites_device_connection(self) -> None:
        """A controller must not power the phone for a rejected adapter contract."""
        events: list[str] = []
        runtime = self.runtime()
        runtime["assets"] = {}
        with self.assertRaisesRegex(SystemExit, "runtime assets"):
            self.run_bridge(BridgeProcess(0), runtime=runtime, events=events.append)
        self.assertEqual(events, [])

    def test_headless_target_serves_only_a_neutral_pinmap(self) -> None:
        """Without maps or display settings, the bootstrap starts headless on an empty pin map."""
        runtime = self.runtime()
        runtime["assets"] = {"fdl1": "assets/fdl1.bin"}
        runtime["adapter"] = headless_adapter_data()
        bridge = BridgeProcess(0)
        output = io.StringIO()
        served: dict[str, bytes] = {}
        directories: list[Path] = []

        def start_bridge(_argv: list[str], *, cwd: Path) -> BridgeProcess:
            directories.append(cwd)
            served.update({path.name: path.read_bytes() for path in cwd.iterdir()})
            return bridge

        popen, _transport, bundle = self.run_bridge(
            bridge,
            runtime=runtime,
            start_bridge=start_bridge,
            output=output,
            bootrom_release_seconds=1.0,
        )

        self.assertEqual(
            popen.call_args.args[0],
            [
                "/usr/bin/stdbuf",
                "-oL",
                "-eL",
                str(bundle / "host/libc_server"),
                "--fplinux-handoff",
                self.session_id,
                "--",
                "--headless",
                "test-linux",
            ],
        )
        self.assertEqual(served, {"pinmap.bin": b"\xff\xff\xff\xff\xff\xff\xff\xff"})
        self.assertEqual(len(directories), 1)
        self.assertFalse(directories[0].exists())
        self.assertIn("Headless target: the phone shows no boot screen", output.getvalue())

    def test_incomplete_map_assets_are_rejected(self) -> None:
        """Pin map and keymap are declared together, and FDL1 is always required."""
        incomplete = (
            {"fdl1": "assets/fdl1.bin", "pinmap": "assets/pinmap.bin"},
            {"fdl1": "assets/fdl1.bin", "keymap": "assets/keymap.bin"},
            {"pinmap": "assets/pinmap.bin", "keymap": "assets/keymap.bin"},
        )
        for assets in incomplete:
            runtime = self.runtime()
            runtime["assets"] = assets
            with (
                self.subTest(assets=sorted(assets)),
                self.assertRaisesRegex(SystemExit, "platform contract"),
            ):
                ADAPTER.run(
                    Path("bundle"), runtime, self.session(), expected_device_identity="9" * 64
                )


if __name__ == "__main__":
    unittest.main()

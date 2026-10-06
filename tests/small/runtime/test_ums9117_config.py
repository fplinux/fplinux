# SPDX-License-Identifier: GPL-2.0-only
"""UMS9117 adapter config behavior."""

from __future__ import annotations

import io
from pathlib import Path

import pytest

from tests.small.runtime.adapter_fixtures import (
    ADAPTER,
    BridgeFixture,
    BridgeProcess,
    adapter_data,
    headless_adapter_data,
    runtime_identity,
)


class RuntimeIdentityTests:
    """Keep platform selection and the rendered target identity fail-closed."""

    def test_reads_the_nested_target_display_name(self) -> None:
        """The adapter renders the validated target identity rather than a root alias."""
        runtime = runtime_identity()
        runtime["display_name"] = "Root Alias Phone"

        assert (ADAPTER.runtime_target_display_name(runtime)) == ("Nokia 3210 4G (TA-1618)")

    def test_rejects_a_bundle_for_another_platform(self) -> None:
        """The fixed adapter refuses a runtime manifest for another platform."""
        with pytest.raises(SystemExit, match="platform identity must name ums9117"):
            ADAPTER.runtime_target_display_name(runtime_identity(platform_name="other"))

    def test_rejects_runtime_without_identity_object(self) -> None:
        """Root display_name and platform keys without an identity object are refused."""
        with pytest.raises(SystemExit, match="runtime identity must be an object"):
            ADAPTER.runtime_target_display_name(
                {"display_name": "Demo Phone", "platform": "ums9117"}
            )


class BacklightConfigurationTests:
    """Reject ambiguous channel spelling and out-of-range levels."""

    def test_canonical_channel_subset_formats_bl_extra(self) -> None:
        """Preserve the selected RGB subset in the bridge argument."""
        config = ADAPTER.adapter_config(
            adapter_data(backlight_channels="rgb", backlight_level=0x1F)
        )
        assert (ADAPTER.backlight_argument(config)) == ("rgb=0x1f")

    @pytest.mark.parametrize(
        "channels",
        [
            pytest.param("", id="empty"),
            pytest.param("gr", id="wrong-order"),
            pytest.param("rr", id="duplicate"),
            pytest.param("rgbwx", id="unknown"),
        ],
    )
    def test_noncanonical_or_invalid_channels_are_rejected(self, channels: str) -> None:
        """Accept only non-empty channels in canonical rgbw order."""
        with (
            pytest.raises(SystemExit, match="backlight_channels"),
        ):
            ADAPTER.adapter_config(adapter_data(backlight_channels=channels))

    def test_backlight_level_above_encoded_range_is_rejected(self) -> None:
        """Keep the level within the six-bit libc_server field."""
        with pytest.raises(SystemExit, match="backlight_level"):
            ADAPTER.adapter_config(adapter_data(backlight_level=0x40))


class DisplaySettingsTests:
    """Accept the loader display settings as one complete set or not at all."""

    @pytest.mark.parametrize(
        "missing",
        [
            pytest.param("spi_mode", id="spi-mode"),
            pytest.param("lcd_id", id="lcd-id"),
            pytest.param("backlight_channels", id="channels"),
            pytest.param("backlight_level", id="level"),
        ],
    )
    def test_incomplete_display_settings_are_rejected(self, missing: str) -> None:
        """A partial LCD description is neither a display nor a headless loader."""
        data = adapter_data()
        del data[missing]
        with (
            pytest.raises(SystemExit, match="adapter data must contain exactly"),
        ):
            ADAPTER.adapter_config(data)

        with pytest.raises(SystemExit, match="adapter data must contain exactly"):
            ADAPTER.adapter_config({**headless_adapter_data(), "lcd_id": 0x8888B6})


class AdapterPreflightTests(BridgeFixture):
    """The loader result, bridge exit status and BootROM release gate the Linux transition."""

    def test_invalid_preflight_never_invites_device_connection(self) -> None:
        """A controller must not power the phone for a rejected adapter contract."""
        events: list[str] = []
        runtime = self.runtime()
        runtime["assets"] = {}
        with pytest.raises(SystemExit, match="runtime assets"):
            self.run_bridge(BridgeProcess(0), runtime=runtime, events=events.append)
        assert (events) == ([])

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

        assert (popen.call_args.args[0]) == (
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
            ]
        )
        assert (served) == ({"pinmap.bin": b"\xff\xff\xff\xff\xff\xff\xff\xff"})
        assert (len(directories)) == (1)
        assert not (directories[0].exists())
        assert ("Headless target: the phone shows no boot screen") in (output.getvalue())

    @pytest.mark.parametrize(
        "assets",
        [
            pytest.param(
                {"fdl1": "assets/fdl1.bin", "pinmap": "assets/pinmap.bin"}, id="missing-keymap"
            ),
            pytest.param(
                {"fdl1": "assets/fdl1.bin", "keymap": "assets/keymap.bin"}, id="missing-pinmap"
            ),
            pytest.param(
                {"pinmap": "assets/pinmap.bin", "keymap": "assets/keymap.bin"}, id="missing-fdl1"
            ),
        ],
    )
    def test_incomplete_map_assets_are_rejected(self, assets: dict[str, str]) -> None:
        """Pin map and keymap are declared together, and FDL1 is always required."""
        runtime = self.runtime()
        runtime["assets"] = assets
        with (
            pytest.raises(SystemExit, match="platform contract"),
        ):
            ADAPTER.run(Path("bundle"), runtime, self.session(), expected_device_identity="9" * 64)

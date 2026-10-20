# SPDX-License-Identifier: GPL-2.0-only
"""UMS9117 adapter config behavior."""

from __future__ import annotations

import io
from pathlib import Path

import pytest

from tests.small.runtime.adapter_fixtures import (
    ADAPTER,
    BRIDGE_SESSION_ID,
    BridgeProcess,
    adapter_data,
    bridge_runtime,
    headless_adapter_data,
    prepared_session,
    run_bridge,
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


@pytest.mark.parametrize("capsule_layout", ["absent", "bundle", "external"])
def test_loader_serves_only_the_selected_capsule_and_releases_its_directory(
    tmp_path: Path, capsule_layout: str
) -> None:
    """The bridge receives the supplied CPIO bytes or an empty owned directory."""
    capsule_parent = tmp_path / "data/phone" if capsule_layout == "external" else tmp_path
    capsule_parent.mkdir(parents=True, exist_ok=True)
    capsule = capsule_parent / "device-data.cpio"
    if capsule_layout != "absent":
        capsule.write_bytes(b"firmware capsule")
    if capsule_layout == "external":
        (tmp_path / "device-data.cpio").write_bytes(b"unselected capsule")

    with ADAPTER.payload_directory(
        tmp_path,
        device_data=capsule if capsule_layout == "external" else None,
    ) as served:
        observed = {path.name: path.read_bytes() for path in served.iterdir()}
        expected = {} if capsule_layout == "absent" else {"device-data.cpio": b"firmware capsule"}
        assert observed == expected
        assert served != capsule_parent

    assert not served.exists()
    if capsule_layout != "absent":
        assert capsule.read_bytes() == b"firmware capsule"


def test_loader_releases_served_capsule_after_consumer_failure(
    tmp_path: Path,
) -> None:
    """A failed bridge releases its staged files while preserving the supplied capsule."""
    capsule = tmp_path / "device-data.cpio"
    capsule.write_bytes(b"firmware capsule")
    served_directory: Path | None = None
    observed: dict[str, bytes] = {}

    def fail_bridge() -> None:
        nonlocal served_directory
        with ADAPTER.payload_directory(tmp_path, device_data=capsule) as served:
            served_directory = served
            observed.update({path.name: path.read_bytes() for path in served.iterdir()})
            message = "controlled bridge failure"
            raise RuntimeError(message)

    with pytest.raises(RuntimeError, match="controlled bridge failure"):
        fail_bridge()

    assert observed == {"device-data.cpio": b"firmware capsule"}
    assert served_directory is not None
    assert not served_directory.exists()
    assert capsule.read_bytes() == b"firmware capsule"


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


class AdapterPreflightTests:
    """The loader result, bridge exit status and BootROM release gate the Linux transition."""

    def test_invalid_preflight_never_invites_device_connection(self) -> None:
        """A controller must not power the phone for a rejected adapter contract."""
        events: list[str] = []
        runtime = bridge_runtime()
        runtime["assets"] = {}
        with pytest.raises(SystemExit, match="runtime assets"):
            run_bridge(BridgeProcess(0), runtime=runtime, events=events.append)
        assert (events) == ([])

    def test_headless_target_uses_an_empty_bridge_directory_without_a_capsule(self) -> None:
        """The headless loader serves no supplemental files and releases its bridge directory."""
        runtime = bridge_runtime()
        runtime["assets"] = {"fdl1": "assets/fdl1.bin"}
        runtime["adapter"] = headless_adapter_data()
        bridge = BridgeProcess(0)
        output = io.StringIO()
        served: dict[str, bytes] = {}
        directories: list[Path] = []

        def start_bridge(
            _argv: list[str], *, cwd: Path, stdout: object = None, stderr: object = None
        ) -> BridgeProcess:
            del stdout, stderr
            directories.append(cwd)
            served.update({path.name: path.read_bytes() for path in cwd.iterdir()})
            return bridge

        popen, _transport, bundle = run_bridge(
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
                BRIDGE_SESSION_ID,
                "--",
                "--headless",
                "test-linux",
            ]
        )
        assert served == {}
        assert (len(directories)) == (1)
        assert not (directories[0].exists())
        assert ("Headless target: the phone shows no boot screen") in (output.getvalue())

    @pytest.mark.parametrize(
        "assets",
        [
            pytest.param({"fdl2": "assets/fdl2.bin"}, id="missing-fdl1"),
            pytest.param(
                {"fdl1": "assets/fdl1.bin", "foreign": "assets/foreign.bin"}, id="foreign-asset"
            ),
        ],
    )
    def test_missing_fdl1_and_foreign_assets_are_rejected(self, assets: dict[str, str]) -> None:
        """The UMS9117 asset contract requires FDL1 and excludes other platforms' stages."""
        runtime = bridge_runtime()
        runtime["assets"] = assets
        with (
            pytest.raises(SystemExit, match="platform contract"),
        ):
            ADAPTER.run(
                Path("bundle"), runtime, prepared_session(), expected_device_identity="9" * 64
            )

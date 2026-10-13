# SPDX-License-Identifier: GPL-2.0-only
"""Component tests for fitted-profile arguments, input identity, and file validation."""

from __future__ import annotations

import hashlib
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.kernel import configuration as kernel_build
from fplinux_cli.build.kernel import receipts as kbuild_state
from fplinux_cli.device_data import inputs as firmware_inputs

if TYPE_CHECKING:
    from collections.abc import Iterator


@dataclass(frozen=True)
class FirmwareProfile:
    """One staged profile declaration and its captured build paths."""

    root: Path
    profile: bytes
    firmware: tuple[firmware_inputs.FirmwareInput, ...]
    profile_path: Path
    directory: Path


@pytest.fixture
def firmware_profile() -> Iterator[FirmwareProfile]:
    """Own a staged profile fixture and its declaration until case teardown."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        profile = b"FPAUDIO\0" + bytes(range(40))
        item = firmware_inputs.FirmwareInput(
            source="inoi240-audio-profile.bin",
            destination="fplinux/inoi240-audio-profile.bin",
            contents=profile,
            sha256=hashlib.sha256(profile).hexdigest(),
        )
        firmware = (item,)
        # Stage the profile where the build workspace places captured device data.
        profile_path = root / firmware_inputs.snapshot_device_data_path(
            "inoi-240-modern-4g", "audio-profile", item.destination
        )
        directory = firmware_inputs.snapshot_device_data_group_directory(
            root, "inoi-240-modern-4g", "audio-profile"
        )
        profile_path.parent.mkdir(parents=True)
        profile_path.write_bytes(profile)
        yield FirmwareProfile(root, profile, firmware, profile_path, directory)


class BuiltinFirmwareTests:
    """Check profile arguments, input identity, and synthetic vmlinux validation."""

    def test_present_group_configures_profile_bytes_and_changes_input_identity(
        self,
        firmware_profile: FirmwareProfile,
    ) -> None:
        """The configured profile changes input identity while adjacent files do not."""
        with mock.patch.object(common, "ROOT", firmware_profile.root):
            arguments = kernel_build.audio_profile_kconfig_arguments(
                "inoi-240-modern-4g",
                firmware_profile.firmware,
            )
            # Kbuild reads EXTRA_FIRMWARE relative to EXTRA_FIRMWARE_DIR.
            configured_bytes = (Path(arguments[5]) / arguments[2]).read_bytes()
            implementation = kernel_build.audio_profile_implementation(
                "inoi-240-modern-4g",
                firmware_profile.firmware,
            )
            first = kbuild_state.implementation_identity(implementation)
            (firmware_profile.directory / "unrelated.bin").write_bytes(b"ignored")
            unrelated = kbuild_state.implementation_identity(implementation)
            firmware_profile.profile_path.write_bytes(b"FPAUDIO\0" + b"X" * 40)
            changed = kbuild_state.implementation_identity(implementation)

        assert (arguments[:5]) == (
            [
                "--set-str",
                "EXTRA_FIRMWARE",
                "fplinux/inoi240-audio-profile.bin",
                "--set-str",
                "EXTRA_FIRMWARE_DIR",
            ]
        )
        assert (configured_bytes) == (firmware_profile.profile)
        assert (first) == (unrelated)
        assert (first) != (changed)

    def test_vmlinux_must_contain_the_exact_configured_profile(
        self, firmware_profile: FirmwareProfile
    ) -> None:
        """The verifier rejects a synthetic vmlinux file missing the configured profile bytes."""
        config_path = firmware_profile.root / ".config"
        config_path.write_text(
            'CONFIG_EXTRA_FIRMWARE="fplinux/inoi240-audio-profile.bin"\n'
            f'CONFIG_EXTRA_FIRMWARE_DIR="{firmware_profile.directory}"\n',
            encoding="utf-8",
        )
        vmlinux = firmware_profile.root / "vmlinux"
        vmlinux.write_bytes(b"prefix" + firmware_profile.profile + b"suffix")

        with mock.patch.object(common, "ROOT", firmware_profile.root):
            kernel_build.verify_builtin_audio_profile(
                "inoi-240-modern-4g",
                config_path,
                vmlinux,
                firmware_profile.firmware,
            )
            vmlinux.write_bytes(b"profile missing")
            with pytest.raises(SystemExit, match="does not embed audio-profile"):
                kernel_build.verify_builtin_audio_profile(
                    "inoi-240-modern-4g",
                    config_path,
                    vmlinux,
                    firmware_profile.firmware,
                )

    def test_absent_group_leaves_the_generic_kconfig_path_unchanged(
        self, firmware_profile: FirmwareProfile
    ) -> None:
        """A whole missing optional profile does not add fitted-firmware arguments."""
        with mock.patch.object(common, "ROOT", firmware_profile.root):
            assert (kernel_build.audio_profile_kconfig_arguments("inoi-240-modern-4g", None)) == (
                []
            )
            assert (kernel_build.audio_profile_implementation("inoi-240-modern-4g", None)) == ([])

    @pytest.mark.parametrize(
        "firmware_setting",
        ["", 'CONFIG_EXTRA_FIRMWARE=""\n'],
        ids=["default-empty", "explicit-empty"],
    )
    def test_ram_configuration_accepts_only_external_firmware(
        self, firmware_profile: FirmwareProfile, firmware_setting: str
    ) -> None:
        """A RAM kernel accepts an empty built-in firmware selection."""
        config_path = firmware_profile.root / ".config"
        config_path.write_text("CONFIG_FW_LOADER=y\n" + firmware_setting, encoding="ascii")

        kernel_build.assert_external_firmware_kconfig(config_path)

    def test_ram_configuration_rejects_embedded_device_data(
        self, firmware_profile: FirmwareProfile
    ) -> None:
        """A RAM kernel rejects profiles that would override separate phone data."""
        config_path = firmware_profile.root / ".config"
        config_path.write_text(
            'CONFIG_EXTRA_FIRMWARE="fplinux/phone-profile.bin"\n', encoding="ascii"
        )

        with pytest.raises(SystemExit, match="RAM kernel unexpectedly contains built-in firmware"):
            kernel_build.assert_external_firmware_kconfig(config_path)

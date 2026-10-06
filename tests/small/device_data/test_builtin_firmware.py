# SPDX-License-Identifier: GPL-2.0-only
"""Component tests for fitted-profile arguments, input identity, and file validation."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import common
from fplinux_cli.build.kernel import configuration as kernel_build
from fplinux_cli.build.kernel import receipts as kbuild_state
from fplinux_cli.device_data import inputs as firmware_inputs


class BuiltinFirmwareTests(unittest.TestCase):
    """Check profile arguments, input identity, and synthetic vmlinux validation."""

    def setUp(self) -> None:
        """Create a staged profile fixture and its declaration."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.profile = b"FPAUDIO\0" + bytes(range(40))
        item = firmware_inputs.FirmwareInput(
            source="inoi240-audio-profile.bin",
            destination="fplinux/inoi240-audio-profile.bin",
            contents=self.profile,
            sha256=hashlib.sha256(self.profile).hexdigest(),
        )
        self.firmware = (item,)
        # Stage the profile where the build workspace places captured device data.
        self.profile_path = self.root / firmware_inputs.snapshot_device_data_path(
            "inoi-240-modern-4g", "audio-profile", item.destination
        )
        self.directory = firmware_inputs.snapshot_device_data_group_directory(
            self.root, "inoi-240-modern-4g", "audio-profile"
        )
        self.profile_path.parent.mkdir(parents=True)
        self.profile_path.write_bytes(self.profile)

    def test_present_group_configures_profile_bytes_and_changes_input_identity(
        self,
    ) -> None:
        """The configured profile changes input identity while adjacent files do not."""
        with mock.patch.object(common, "ROOT", self.root):
            arguments = kernel_build.audio_profile_kconfig_arguments(
                "inoi-240-modern-4g",
                self.firmware,
            )
            # Kbuild reads EXTRA_FIRMWARE relative to EXTRA_FIRMWARE_DIR.
            configured_bytes = (Path(arguments[5]) / arguments[2]).read_bytes()
            implementation = kernel_build.audio_profile_implementation(
                "inoi-240-modern-4g",
                self.firmware,
            )
            first = kbuild_state.implementation_identity(implementation)
            (self.directory / "unrelated.bin").write_bytes(b"ignored")
            unrelated = kbuild_state.implementation_identity(implementation)
            self.profile_path.write_bytes(b"FPAUDIO\0" + b"X" * 40)
            changed = kbuild_state.implementation_identity(implementation)

        self.assertEqual(
            arguments[:5],
            [
                "--set-str",
                "EXTRA_FIRMWARE",
                "fplinux/inoi240-audio-profile.bin",
                "--set-str",
                "EXTRA_FIRMWARE_DIR",
            ],
        )
        self.assertEqual(configured_bytes, self.profile)
        self.assertEqual(first, unrelated)
        self.assertNotEqual(first, changed)

    def test_vmlinux_must_contain_the_exact_configured_profile(self) -> None:
        """The verifier rejects a synthetic vmlinux file missing the configured profile bytes."""
        config_path = self.root / ".config"
        config_path.write_text(
            'CONFIG_EXTRA_FIRMWARE="fplinux/inoi240-audio-profile.bin"\n'
            f'CONFIG_EXTRA_FIRMWARE_DIR="{self.directory}"\n',
            encoding="utf-8",
        )
        vmlinux = self.root / "vmlinux"
        vmlinux.write_bytes(b"prefix" + self.profile + b"suffix")

        with mock.patch.object(common, "ROOT", self.root):
            kernel_build.verify_builtin_audio_profile(
                "inoi-240-modern-4g",
                config_path,
                vmlinux,
                self.firmware,
            )
            vmlinux.write_bytes(b"profile missing")
            with self.assertRaisesRegex(SystemExit, "does not embed audio-profile"):
                kernel_build.verify_builtin_audio_profile(
                    "inoi-240-modern-4g",
                    config_path,
                    vmlinux,
                    self.firmware,
                )

    def test_absent_group_leaves_the_generic_kconfig_path_unchanged(self) -> None:
        """A whole missing optional profile does not add fitted-firmware arguments."""
        with mock.patch.object(common, "ROOT", self.root):
            self.assertEqual(
                kernel_build.audio_profile_kconfig_arguments("inoi-240-modern-4g", None),
                [],
            )
            self.assertEqual(
                kernel_build.audio_profile_implementation("inoi-240-modern-4g", None),
                [],
            )


if __name__ == "__main__":
    unittest.main()

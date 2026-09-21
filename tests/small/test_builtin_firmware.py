# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for native Kbuild delivery of the optional fitted profile."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import common, firmware_inputs, kbuild_state
from fplinux_cli.build import kernel as kernel_build


class BuiltinFirmwareTests(unittest.TestCase):
    """Bind the exact private bytes to Kconfig, Kbuild identity, and vmlinux."""

    def setUp(self) -> None:
        """Create one immutable-looking workspace profile and its declaration."""
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
        self.profile_identity = (
            ".fplinux-inputs/device-data/inoi-240-modern-4g/groups/"
            "audio-profile/fplinux/inoi240-audio-profile.bin"
        )
        self.profile_path = self.root / self.profile_identity
        self.directory = self.root / (
            ".fplinux-inputs/device-data/inoi-240-modern-4g/groups/audio-profile"
        )
        self.profile_path.parent.mkdir(parents=True)
        self.profile_path.write_bytes(self.profile)

    def test_present_group_uses_native_kconfig_and_changes_kbuild_identity(
        self,
    ) -> None:
        """The admitted profile, not adjacent files, is a causal Kbuild input."""
        with mock.patch.object(common, "ROOT", self.root):
            arguments = kernel_build.audio_profile_kconfig_arguments(
                "inoi-240-modern-4g",
                self.firmware,
            )
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
            arguments,
            [
                "--set-str",
                "EXTRA_FIRMWARE",
                "fplinux/inoi240-audio-profile.bin",
                "--set-str",
                "EXTRA_FIRMWARE_DIR",
                str(self.directory),
            ],
        )
        self.assertEqual(implementation, [(self.profile_identity, self.profile_path)])
        self.assertEqual(first, unrelated)
        self.assertNotEqual(first, changed)

    def test_vmlinux_must_contain_the_exact_configured_profile(self) -> None:
        """A fitted build cannot proceed after Kbuild loses the embedded bytes."""
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

# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for the content-derived target kernel local version."""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import device_state


class DeviceStateTests(unittest.TestCase):
    """Require LOCALVERSION to follow device/kernel inputs, never the bundle workspace."""

    def setUp(self) -> None:
        """Create one complete temporary target-kernel input closure."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.defconfig = Path(self.temporary.name) / "defconfig"
        self.defconfig.write_text("CONFIG_DEMO=y\n", encoding="utf-8")
        self.target = "demo-device"
        self.linux_recipe = "2" * 64
        self.bootstrap_recipe = "3" * 64
        self.rootfs: dict[str, object] = {"sha256": "1" * 64, "size": 1234}
        self.buildroot_receipt: dict[str, object] = {
            "recipe": "7" * 64,
            "sha256": "8" * 64,
        }
        self.arch = "arm"
        self.dtb = "vendor/demo-device.dtb"

    def _identity(self) -> str:
        return device_state.device_kernel_identity(
            target=self.target,
            linux_recipe=self.linux_recipe,
            bootstrap_recipe=self.bootstrap_recipe,
            rootfs=self.rootfs,
            buildroot_receipt=self.buildroot_receipt,
            arch=self.arch,
            defconfig=self.defconfig,
            dtb=self.dtb,
        )

    def test_identity_is_deterministic_and_formats_a_kernel_localversion(self) -> None:
        """One unchanged target closure has one stable full identity and suffix."""
        identity = self._identity()

        self.assertEqual(identity, self._identity())
        self.assertRegex(identity, r"^[0-9a-f]{64}$")
        self.assertEqual(device_state.localversion(identity), "-fplinux-" + identity[:16])

    def test_workspace_digest_does_not_change_the_device_kernel_identity(self) -> None:
        """Host-only bundle orchestration cannot relabel an otherwise unchanged kernel."""
        with mock.patch.dict(
            os.environ,
            {"FPLINUX_WORKSPACE_DIGEST": "3" * 64},
            clear=False,
        ):
            first = self._identity()
        with mock.patch.dict(
            os.environ,
            {"FPLINUX_WORKSPACE_DIGEST": "4" * 64},
            clear=False,
        ):
            second = self._identity()

        self.assertEqual(first, second)

    def test_defconfig_metadata_does_not_change_the_device_kernel_identity(self) -> None:
        """Only defconfig bytes, not host filesystem metadata, are causal."""
        before = self._identity()
        os.utime(self.defconfig, ns=(1_000_000_000, 1_000_000_000))
        self.assertEqual(before, self._identity())

    def test_actual_linux_rootfs_and_target_kernel_inputs_change_identity(self) -> None:
        """Each pre-Kbuild input that changes device-visible kernel content is causal."""
        before = self._identity()

        self.linux_recipe = "5" * 64
        self.assertNotEqual(before, self._identity())
        self.linux_recipe = "2" * 64

        self.bootstrap_recipe = "4" * 64
        self.assertNotEqual(before, self._identity())
        self.bootstrap_recipe = "3" * 64

        self.rootfs = {"sha256": "6" * 64, "size": 1234}
        self.assertNotEqual(before, self._identity())
        self.rootfs = {"sha256": "1" * 64, "size": 1234}

        self.buildroot_receipt = {"recipe": "9" * 64, "sha256": "8" * 64}
        self.assertNotEqual(before, self._identity())
        self.buildroot_receipt = {"recipe": "7" * 64, "sha256": "8" * 64}

        self.arch = "arm64"
        self.assertNotEqual(before, self._identity())
        self.arch = "arm"

        self.defconfig.write_text("CONFIG_DEMO=n\n", encoding="utf-8")
        self.assertNotEqual(before, self._identity())
        self.defconfig.write_text("CONFIG_DEMO=y\n", encoding="utf-8")

        self.dtb = "vendor/other-device.dtb"
        self.assertNotEqual(before, self._identity())
        self.dtb = "vendor/demo-device.dtb"

        self.target = "other-device"
        self.assertNotEqual(before, self._identity())

    def test_invalid_identity_inputs_fail_closed(self) -> None:
        """The helper cannot silently label a kernel from an incomplete closure."""
        with self.assertRaisesRegex(device_state.DeviceStateError, "rootfs identity"):
            device_state.device_kernel_identity(
                target=self.target,
                linux_recipe=self.linux_recipe,
                bootstrap_recipe=self.bootstrap_recipe,
                rootfs={"sha256": "1" * 64},
                buildroot_receipt=self.buildroot_receipt,
                arch=self.arch,
                defconfig=self.defconfig,
                dtb=self.dtb,
            )
        with self.assertRaisesRegex(device_state.DeviceStateError, "target DTB"):
            device_state.device_kernel_identity(
                target=self.target,
                linux_recipe=self.linux_recipe,
                bootstrap_recipe=self.bootstrap_recipe,
                rootfs=self.rootfs,
                buildroot_receipt=self.buildroot_receipt,
                arch=self.arch,
                defconfig=self.defconfig,
                dtb="../unsafe.dtb",
            )
        with self.assertRaisesRegex(device_state.DeviceStateError, "device/kernel identity"):
            device_state.localversion("not-a-digest")

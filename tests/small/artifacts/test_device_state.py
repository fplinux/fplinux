# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the content-derived target kernel identity and local version."""

from __future__ import annotations

import os
import re
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.kernel import identity as device_state
from fplinux_cli.build.kernel import receipts as kbuild_state

if TYPE_CHECKING:
    from pathlib import Path


class DeviceStateTests:
    """Require the kernel identity to follow the content of its device/kernel inputs."""

    @pytest.fixture(autouse=True)
    def _kernel_inputs(self, tmp_path: Path) -> None:
        """Create one complete temporary target-kernel input closure."""
        self.defconfig = tmp_path / "defconfig"
        self.defconfig.write_text("CONFIG_DEMO=y\n", encoding="utf-8")
        self.target = "demo-device"
        self.linux_recipe = "2" * 64
        self.bootstrap_recipe = "3" * 64
        self.root: dict[str, object] = {
            "kind": "initramfs",
            "artifact": {"sha256": "1" * 64, "size": 1234},
            "receipt": {"recipe": "7" * 64, "sha256": "8" * 64},
        }
        self.implementation = tmp_path / "implementation.py"
        self.implementation.write_text("first\n", encoding="utf-8")
        self.arch = "arm"
        self.dtb = "vendor/demo-device.dtb"

    def _identity(self, *, build_type: str = "release") -> str:
        return device_state.device_kernel_identity(
            target=self.target,
            linux_recipe=self.linux_recipe,
            bootstrap_recipe=self.bootstrap_recipe,
            root=self.root,
            kbuild_implementation=kbuild_state.implementation_identity(
                [("scripts/implementation.py", self.implementation)]
            ),
            arch=self.arch,
            defconfig=self.defconfig,
            dtb=self.dtb,
            build_type=build_type,
        )

    def test_build_type_changes_the_kernel_identity(self) -> None:
        """Release and debug selections of identical inputs get distinct, stable identities."""
        release = self._identity()
        debug = self._identity(build_type="debug")
        assert (release) != (debug)
        assert (release) == (self._identity(build_type="release"))

    def test_identity_is_deterministic_and_formats_a_kernel_localversion(self) -> None:
        """One unchanged target closure has one stable full identity and suffix."""
        identity = self._identity()

        assert (identity) == (self._identity())
        assert re.search(r"^[0-9a-f]{64}$", identity) is not None
        assert (device_state.localversion(identity)) == ("-fplinux-" + identity[:16])

    def test_defconfig_metadata_does_not_change_the_device_kernel_identity(self) -> None:
        """Only defconfig bytes, not host filesystem metadata, are causal."""
        before = self._identity()
        os.utime(self.defconfig, ns=(1_000_000_000, 1_000_000_000))
        assert (before) == (self._identity())

    def test_actual_linux_root_and_target_kernel_inputs_change_identity(self) -> None:
        """Each pre-Kbuild input that changes device-visible kernel content is causal."""
        before = self._identity()

        self.linux_recipe = "5" * 64
        assert (before) != (self._identity())
        self.linux_recipe = "2" * 64

        self.bootstrap_recipe = "4" * 64
        assert (before) != (self._identity())
        self.bootstrap_recipe = "3" * 64

        self.root = {
            "kind": "initramfs",
            "artifact": {"sha256": "6" * 64, "size": 1234},
            "receipt": {"recipe": "7" * 64, "sha256": "8" * 64},
        }
        assert (before) != (self._identity())
        self.root = {
            "kind": "initramfs",
            "artifact": {"sha256": "1" * 64, "size": 1234},
            "receipt": {"recipe": "7" * 64, "sha256": "8" * 64},
        }

        self.root["receipt"] = {"recipe": "9" * 64, "sha256": "8" * 64}
        assert (before) != (self._identity())
        self.root["receipt"] = {"recipe": "7" * 64, "sha256": "8" * 64}

        self.arch = "arm64"
        assert (before) != (self._identity())
        self.arch = "arm"

        self.defconfig.write_text("CONFIG_DEMO=n\n", encoding="utf-8")
        assert (before) != (self._identity())
        self.defconfig.write_text("CONFIG_DEMO=y\n", encoding="utf-8")

        self.dtb = "vendor/other-device.dtb"
        assert (before) != (self._identity())
        self.dtb = "vendor/demo-device.dtb"

        self.target = "other-device"
        assert (before) != (self._identity())

        self.target = "demo-device"
        self.implementation.write_text("second\n", encoding="utf-8")
        assert (before) != (self._identity())

    def test_named_profile_and_its_kconfig_actions_change_identity(self) -> None:
        """Different profile selections must never share a device kernel suffix."""
        default = self._identity()
        common: dict[str, Any] = {
            "target": self.target,
            "linux_recipe": self.linux_recipe,
            "bootstrap_recipe": self.bootstrap_recipe,
            "root": self.root,
            "kbuild_implementation": kbuild_state.implementation_identity(
                [("scripts/implementation.py", self.implementation)]
            ),
            "arch": self.arch,
            "defconfig": self.defconfig,
            "dtb": self.dtb,
        }
        named = device_state.device_kernel_identity(
            **common,
            profile="usb-host-lab",
            config_enable=("CONFIG_USB",),
            config_disable=("CONFIG_USB_GADGET",),
        )
        changed_actions = device_state.device_kernel_identity(
            **common,
            profile="usb-host-lab",
            config_enable=("CONFIG_USB_HID",),
            config_disable=("CONFIG_USB_GADGET",),
        )

        assert (default) != (named)
        assert (named) != (changed_actions)

    def test_invalid_identity_inputs_fail_closed(self) -> None:
        """The helper cannot silently label a kernel from an incomplete closure."""
        with pytest.raises(device_state.DeviceStateError, match="initramfs root"):
            device_state.device_kernel_identity(
                target=self.target,
                linux_recipe=self.linux_recipe,
                bootstrap_recipe=self.bootstrap_recipe,
                root={"kind": "initramfs", "artifact": {"sha256": "1" * 64}},
                kbuild_implementation="9" * 64,
                arch=self.arch,
                defconfig=self.defconfig,
                dtb=self.dtb,
            )
        with pytest.raises(device_state.DeviceStateError, match="target DTB"):
            device_state.device_kernel_identity(
                target=self.target,
                linux_recipe=self.linux_recipe,
                bootstrap_recipe=self.bootstrap_recipe,
                root=self.root,
                kbuild_implementation="9" * 64,
                arch=self.arch,
                defconfig=self.defconfig,
                dtb="../unsafe.dtb",
            )
        with pytest.raises(device_state.DeviceStateError, match="device/kernel identity"):
            device_state.localversion("not-a-digest")

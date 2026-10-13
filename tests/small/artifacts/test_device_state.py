# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the content-derived target kernel identity and local version."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.kernel import identity as device_state
from fplinux_cli.build.kernel import receipts as kbuild_state

if TYPE_CHECKING:
    from pathlib import Path


@dataclass
class KernelInputs:
    """Mutable target-kernel inputs owned by one identity scenario."""

    defconfig: Path
    target: str
    linux_recipe: str
    bootstrap_recipe: str
    root: dict[str, object]
    implementation: Path
    arch: str
    dtb: str


@pytest.fixture
def kernel_inputs(tmp_path: Path) -> KernelInputs:
    """Create one complete temporary target-kernel input closure."""
    defconfig = tmp_path / "defconfig"
    defconfig.write_text("CONFIG_DEMO=y\n", encoding="utf-8")
    implementation = tmp_path / "implementation.py"
    implementation.write_text("first\n", encoding="utf-8")
    return KernelInputs(
        defconfig=defconfig,
        target="demo-device",
        linux_recipe="2" * 64,
        bootstrap_recipe="3" * 64,
        root={
            "kind": "initramfs",
            "artifact": {"sha256": "1" * 64, "size": 1234},
            "receipt": {"recipe": "7" * 64, "sha256": "8" * 64},
        },
        implementation=implementation,
        arch="arm",
        dtb="vendor/demo-device.dtb",
    )


def kernel_identity(kernel_inputs: KernelInputs, *, build_type: str = "release") -> str:
    return device_state.device_kernel_identity(
        target=kernel_inputs.target,
        linux_recipe=kernel_inputs.linux_recipe,
        bootstrap_recipe=kernel_inputs.bootstrap_recipe,
        root=kernel_inputs.root,
        kbuild_implementation=kbuild_state.implementation_identity(
            [("scripts/implementation.py", kernel_inputs.implementation)]
        ),
        arch=kernel_inputs.arch,
        defconfig=kernel_inputs.defconfig,
        dtb=kernel_inputs.dtb,
        build_type=build_type,
    )


class DeviceStateTests:
    """Require the kernel identity to follow the content of its device/kernel inputs."""

    def test_build_type_changes_the_kernel_identity(self, kernel_inputs: KernelInputs) -> None:
        """Release and debug selections of identical inputs get distinct, stable identities."""
        release = kernel_identity(kernel_inputs)
        debug = kernel_identity(kernel_inputs, build_type="debug")
        assert (release) != (debug)
        assert (release) == (kernel_identity(kernel_inputs, build_type="release"))

    def test_identity_is_deterministic_and_formats_a_kernel_localversion(
        self, kernel_inputs: KernelInputs
    ) -> None:
        """One unchanged target closure has one stable full identity and suffix."""
        identity = kernel_identity(kernel_inputs)

        assert (identity) == (kernel_identity(kernel_inputs))
        assert re.search(r"^[0-9a-f]{64}$", identity) is not None
        assert (device_state.localversion(identity)) == ("-fplinux-" + identity[:16])

    def test_defconfig_metadata_does_not_change_the_device_kernel_identity(
        self, kernel_inputs: KernelInputs
    ) -> None:
        """Only defconfig bytes, not host filesystem metadata, are causal."""
        before = kernel_identity(kernel_inputs)
        os.utime(kernel_inputs.defconfig, ns=(1_000_000_000, 1_000_000_000))
        assert (before) == (kernel_identity(kernel_inputs))

    def test_actual_linux_root_and_target_kernel_inputs_change_identity(
        self, kernel_inputs: KernelInputs
    ) -> None:
        """Each pre-Kbuild input that changes device-visible kernel content is causal."""
        before = kernel_identity(kernel_inputs)

        kernel_inputs.linux_recipe = "5" * 64
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.linux_recipe = "2" * 64

        kernel_inputs.bootstrap_recipe = "4" * 64
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.bootstrap_recipe = "3" * 64

        kernel_inputs.root = {
            "kind": "initramfs",
            "artifact": {"sha256": "6" * 64, "size": 1234},
            "receipt": {"recipe": "7" * 64, "sha256": "8" * 64},
        }
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.root = {
            "kind": "initramfs",
            "artifact": {"sha256": "1" * 64, "size": 1234},
            "receipt": {"recipe": "7" * 64, "sha256": "8" * 64},
        }

        kernel_inputs.root["receipt"] = {"recipe": "9" * 64, "sha256": "8" * 64}
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.root["receipt"] = {"recipe": "7" * 64, "sha256": "8" * 64}

        kernel_inputs.arch = "arm64"
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.arch = "arm"

        kernel_inputs.defconfig.write_text("CONFIG_DEMO=n\n", encoding="utf-8")
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.defconfig.write_text("CONFIG_DEMO=y\n", encoding="utf-8")

        kernel_inputs.dtb = "vendor/other-device.dtb"
        assert (before) != (kernel_identity(kernel_inputs))
        kernel_inputs.dtb = "vendor/demo-device.dtb"

        kernel_inputs.target = "other-device"
        assert (before) != (kernel_identity(kernel_inputs))

        kernel_inputs.target = "demo-device"
        kernel_inputs.implementation.write_text("second\n", encoding="utf-8")
        assert (before) != (kernel_identity(kernel_inputs))

    def test_named_profile_and_its_kconfig_actions_change_identity(
        self, kernel_inputs: KernelInputs
    ) -> None:
        """Different profile selections must never share a device kernel suffix."""
        default = kernel_identity(kernel_inputs)
        common: dict[str, Any] = {
            "target": kernel_inputs.target,
            "linux_recipe": kernel_inputs.linux_recipe,
            "bootstrap_recipe": kernel_inputs.bootstrap_recipe,
            "root": kernel_inputs.root,
            "kbuild_implementation": kbuild_state.implementation_identity(
                [("scripts/implementation.py", kernel_inputs.implementation)]
            ),
            "arch": kernel_inputs.arch,
            "defconfig": kernel_inputs.defconfig,
            "dtb": kernel_inputs.dtb,
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

    def test_invalid_identity_inputs_fail_closed(self, kernel_inputs: KernelInputs) -> None:
        """The helper cannot silently label a kernel from an incomplete closure."""
        with pytest.raises(device_state.DeviceStateError, match="initramfs root"):
            device_state.device_kernel_identity(
                target=kernel_inputs.target,
                linux_recipe=kernel_inputs.linux_recipe,
                bootstrap_recipe=kernel_inputs.bootstrap_recipe,
                root={"kind": "initramfs", "artifact": {"sha256": "1" * 64}},
                kbuild_implementation="9" * 64,
                arch=kernel_inputs.arch,
                defconfig=kernel_inputs.defconfig,
                dtb=kernel_inputs.dtb,
            )
        with pytest.raises(device_state.DeviceStateError, match="target DTB"):
            device_state.device_kernel_identity(
                target=kernel_inputs.target,
                linux_recipe=kernel_inputs.linux_recipe,
                bootstrap_recipe=kernel_inputs.bootstrap_recipe,
                root=kernel_inputs.root,
                kbuild_implementation="9" * 64,
                arch=kernel_inputs.arch,
                defconfig=kernel_inputs.defconfig,
                dtb="../unsafe.dtb",
            )
        with pytest.raises(device_state.DeviceStateError, match="device/kernel identity"):
            device_state.localversion("not-a-digest")

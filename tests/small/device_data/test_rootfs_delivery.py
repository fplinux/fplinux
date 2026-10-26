# SPDX-License-Identifier: GPL-2.0-only
"""Firmware delivery into the SC9820E rootfs and unchanged capsule consumers."""

from __future__ import annotations

import hashlib
import lzma
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine.recipes import alpine_rootfs_recipe
from fplinux_cli.device_data.inputs import (
    FirmwareInput,
    install_firmware_inputs,
    rootfs_firmware_inputs,
    rootfs_receives_firmware,
    verify_installed_firmware_inputs,
)

if TYPE_CHECKING:
    from pathlib import Path


@pytest.mark.parametrize(
    ("platform", "root_kind", "expected"),
    [
        ("sc9820e", "initramfs", True),
        ("ums9117", "initramfs", False),
        ("ums9117", "external", True),
    ],
)
def test_delivery_respects_the_supported_loader_boundary(
    platform: str, root_kind: str, *, expected: bool
) -> None:
    """SC9820E has one built rootfs while UMS9117 RAM boot retains its capsule."""
    assert rootfs_receives_firmware(root_kind=root_kind, platform=platform) is expected


def test_qin_ram_rootfs_installs_payload_and_firmware_changes_recipe(tmp_path: Path) -> None:
    """Real firmware installation and recipe identity consume the selected radio bytes."""
    contents = b"Qin radio template\n" * 128
    firmware = FirmwareInput(
        source="system-config.bin",
        destination="qin1splus/system-config.bin",
        contents=contents,
        sha256=hashlib.sha256(contents).hexdigest(),
    )
    groups = {"wcn-firmware": (firmware,)}
    captured = (
        rootfs_firmware_inputs(groups)
        if rootfs_receives_firmware(root_kind="initramfs", platform="sc9820e")
        else ()
    )
    root = tmp_path / "root"
    root.mkdir()

    install_firmware_inputs(root, captured)

    verify_installed_firmware_inputs(root, captured)
    installed = root / "lib/firmware/qin1splus/system-config.bin.xz"
    assert lzma.decompress(installed.read_bytes(), format=lzma.FORMAT_XZ) == contents
    assert installed.stat().st_mode & 0o777 == 0o600
    assert not (root / "lib/firmware/qin1splus/system-config.bin").exists()
    with_firmware = alpine_rootfs_recipe(
        "a" * 64,
        "b" * 64,
        (),
        release_id="012345abcdef",
        firmware_inputs=captured,
        root_kind="external",
    )
    without_firmware = alpine_rootfs_recipe(
        "a" * 64,
        "b" * 64,
        (),
        release_id="012345abcdef",
        firmware_inputs=(),
        root_kind="external",
    )
    assert with_firmware != without_firmware

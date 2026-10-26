# SPDX-License-Identifier: GPL-2.0-only
"""Component checks for selected profile bytes, causal identity and kernel verification."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, replace
from pathlib import Path

import pytest
from fplinux_cli.build.kernel import configuration as kernel_build
from fplinux_cli.build.kernel import receipts as kbuild_state
from fplinux_cli.device_data import inputs as firmware_inputs


@dataclass(frozen=True)
class FirmwareProfile:
    """One admitted immutable audio group and the work directory owned by its test."""

    root: Path
    profile: bytes
    firmware: tuple[firmware_inputs.FirmwareInput, ...]


@pytest.fixture
def firmware_profile(tmp_path: Path) -> FirmwareProfile:
    """Supply captured profile bytes without an ambient phone store or build cache."""
    profile = b"FPAUDIO\0" + bytes(range(40))
    item = firmware_inputs.FirmwareInput(
        source="inoi240-audio-profile.bin",
        destination="fplinux/inoi240-audio-profile.bin",
        contents=profile,
        sha256=hashlib.sha256(profile).hexdigest(),
    )
    return FirmwareProfile(tmp_path, profile, (item,))


def test_present_profile_changes_causal_identity_while_adjacent_files_do_not(
    firmware_profile: FirmwareProfile,
) -> None:
    """The actual selected contents and names participate in the Kbuild receipt."""
    config = {"display": {"panel": None}, "linux": {"root": {"kind": "external"}}}
    selected = kernel_build.prepare_builtin_firmware(
        config, firmware_profile.firmware, source=firmware_profile.root, work=firmware_profile.root
    )
    arguments = selected.kconfig_arguments()
    configured_bytes = (Path(arguments[5]) / arguments[2]).read_bytes()
    implementation = selected.implementation()
    first = kbuild_state.implementation_identity(implementation)
    (selected.directory / "unrelated.bin").write_bytes(b"ignored")
    unrelated = kbuild_state.implementation_identity(implementation)
    changed_profile = b"FPAUDIO\0" + b"X" * 40
    changed = replace(
        firmware_profile.firmware[0],
        contents=changed_profile,
        sha256=hashlib.sha256(changed_profile).hexdigest(),
    )
    changed_selection = kernel_build.prepare_builtin_firmware(
        config, (changed,), source=firmware_profile.root, work=firmware_profile.root
    )
    changed_identity = kbuild_state.implementation_identity(changed_selection.implementation())

    assert arguments == [
        "--set-str",
        "EXTRA_FIRMWARE",
        "fplinux/inoi240-audio-profile.bin",
        "--set-str",
        "EXTRA_FIRMWARE_DIR",
        str(firmware_profile.root / "builtin-firmware"),
    ]
    assert configured_bytes == firmware_profile.profile
    assert first == unrelated
    assert first != changed_identity


def test_vmlinux_must_contain_the_exact_selected_profile(
    firmware_profile: FirmwareProfile,
) -> None:
    """Configuration alone cannot admit an artifact that lost the fitted profile."""
    selected = kernel_build.prepare_builtin_firmware(
        {"display": {"panel": None}, "linux": {"root": {"kind": "external"}}},
        firmware_profile.firmware,
        source=firmware_profile.root,
        work=firmware_profile.root,
    )
    config = firmware_profile.root / ".config"
    config.write_text(
        'CONFIG_EXTRA_FIRMWARE="fplinux/inoi240-audio-profile.bin"\n'
        f'CONFIG_EXTRA_FIRMWARE_DIR="{firmware_profile.root / "builtin-firmware"}"\n',
        encoding="ascii",
    )
    vmlinux = firmware_profile.root / "vmlinux"
    vmlinux.write_bytes(b"prefix" + firmware_profile.profile + b"suffix")
    selected.verify(config, vmlinux)
    vmlinux.write_bytes(b"profile missing")
    with pytest.raises(SystemExit, match="does not embed firmware input"):
        selected.verify(config, vmlinux)


def test_absent_profile_clears_the_kernel_firmware_selection(
    firmware_profile: FirmwareProfile,
) -> None:
    selected = kernel_build.prepare_builtin_firmware(
        {"display": {"panel": None}, "linux": {"root": {"kind": "external"}}},
        None,
        source=firmware_profile.root,
        work=firmware_profile.root,
    )
    assert selected.kconfig_arguments() == [
        "--set-str",
        "EXTRA_FIRMWARE",
        "",
        "--set-str",
        "EXTRA_FIRMWARE_DIR",
        "",
    ]
    assert selected.implementation() == []


@pytest.mark.parametrize("firmware_setting", ["", 'CONFIG_EXTRA_FIRMWARE=""\n'])
def test_ram_without_a_panel_accepts_only_empty_builtin_firmware(
    firmware_profile: FirmwareProfile, firmware_setting: str
) -> None:
    """RAM audio data remains in its supplemental capsule even when a group is captured."""
    selected = kernel_build.prepare_builtin_firmware(
        {"display": {"panel": None}, "linux": {"root": {"kind": "initramfs"}}},
        firmware_profile.firmware,
        source=firmware_profile.root,
        work=firmware_profile.root,
    )
    assert selected.files == ()
    config = firmware_profile.root / ".config"
    config.write_text("CONFIG_FW_LOADER=y\n" + firmware_setting, encoding="ascii")
    selected.verify(config, firmware_profile.root / "unused-vmlinux")


def test_ram_configuration_rejects_embedded_device_data(firmware_profile: FirmwareProfile) -> None:
    selected = kernel_build.prepare_builtin_firmware(
        {"display": {"panel": None}, "linux": {"root": {"kind": "initramfs"}}},
        firmware_profile.firmware,
        source=firmware_profile.root,
        work=firmware_profile.root,
    )
    config = firmware_profile.root / ".config"
    config.write_text('CONFIG_EXTRA_FIRMWARE="fplinux/phone-profile.bin"\n', encoding="ascii")
    with pytest.raises(SystemExit, match="configuration lost its built-in firmware selection"):
        selected.verify(config, firmware_profile.root / "unused-vmlinux")

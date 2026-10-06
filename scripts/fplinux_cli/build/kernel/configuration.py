# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and validate profile, build-type and embedded-firmware Kconfig inputs."""

from __future__ import annotations

import mmap
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.common import fail
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.manifests.kernel import compose_kernel_config, kconfig_values, kernel_config_paths

if TYPE_CHECKING:
    from pathlib import Path


def prepare_kernel_config(
    target: str, target_config: dict[str, Any], platform: dict[str, Any], work: Path
) -> tuple[Path, tuple[Path, Path, Path]]:
    """Write the effective configuration and retain its selected policy inputs."""
    config_paths = kernel_config_paths(target, target_config, platform)
    work.mkdir(parents=True, exist_ok=True)
    defconfig = work / "kernel.defconfig"
    defconfig.write_bytes(compose_kernel_config(*config_paths))
    return defconfig, config_paths


def profile_kconfig_actions(target_config: dict[str, Any]) -> tuple[list[str], list[str]]:
    """Return the profile-only scripts/config actions in declared order."""
    linux = target_config["linux"]
    enable = linux.get("config_enable", [])
    disable = linux.get("config_disable", [])
    if (
        not isinstance(enable, list)
        or not isinstance(disable, list)
        or not all(isinstance(symbol, str) and symbol for symbol in [*enable, *disable])
    ):
        fail("target profile Kconfig actions are invalid")
    return list(enable), list(disable)


def profile_kconfig_arguments(config_enable: list[str], config_disable: list[str]) -> list[str]:
    """Render declared CONFIG_* actions for the Linux scripts/config command."""
    arguments: list[str] = []
    for action, symbols in (("--enable", config_enable), ("--disable", config_disable)):
        for symbol in symbols:
            if not symbol.startswith("CONFIG_") or len(symbol) == len("CONFIG_"):
                fail("target profile Kconfig symbol is invalid")
            arguments.extend((action, symbol.removeprefix("CONFIG_")))
    return arguments


def assert_profile_kconfig(
    config: Path, config_enable: list[str], config_disable: list[str]
) -> None:
    """Require selected profile Kconfig values after dependency resolution."""
    values = kconfig_values(inputs_build.require_file(config).read_text())
    for symbol in config_enable:
        if values.get(symbol, "n") != "y":
            fail(f"profile did not enable {symbol}")
    for symbol in config_disable:
        if values.get(symbol, "n") != "n":
            fail(f"profile did not disable {symbol}")


def assert_build_type_kconfig(config: Path, fragment: Path, build_type: str) -> None:
    """Require the selected type's capabilities after Kconfig dependency resolution."""
    requested = kconfig_values(inputs_build.require_file(fragment).read_text())
    actual = kconfig_values(inputs_build.require_file(config).read_text())
    for symbol, value in requested.items():
        if actual.get(symbol, "n") != value:
            fail(f"{build_type} build did not preserve {symbol}={value}")


def audio_profile_kconfig_arguments(
    target: str,
    firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
) -> list[str]:
    """Embed the fitted audio profile only when its complete group is present."""
    if firmware is None:
        return []
    directory = firmware_inputs.snapshot_device_data_group_directory(
        common.ROOT,
        target,
        "audio-profile",
    )
    names = " ".join(item.destination for item in firmware)
    return [
        "--set-str",
        "EXTRA_FIRMWARE",
        names,
        "--set-str",
        "EXTRA_FIRMWARE_DIR",
        str(directory),
    ]


def audio_profile_implementation(
    target: str,
    firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
) -> list[tuple[str, Path]]:
    """Expose exact fitted-profile bytes to the Kbuild causal-input receipt."""
    if firmware is None:
        return []
    records = []
    for item in firmware:
        relative = firmware_inputs.snapshot_device_data_path(
            target,
            "audio-profile",
            item.destination,
        )
        records.append((relative, common.ROOT / relative))
    return records


def _file_contains(path: Path, contents: bytes) -> bool:
    """Search a built artifact without copying the complete file into host memory."""
    with (
        path.open("rb") as stream,
        mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image,
    ):
        return image.find(contents) >= 0


def verify_builtin_audio_profile(
    target: str,
    config_path: Path,
    vmlinux: Path,
    firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
) -> None:
    """Require the configured fitted-profile names and exact bytes in vmlinux."""
    if firmware is None:
        return
    directory = firmware_inputs.snapshot_device_data_group_directory(
        common.ROOT,
        target,
        "audio-profile",
    )
    names = " ".join(item.destination for item in firmware)
    config_text = config_path.read_text()
    for expected in (
        f'CONFIG_EXTRA_FIRMWARE="{names}"',
        f'CONFIG_EXTRA_FIRMWARE_DIR="{directory}"',
    ):
        if expected not in config_text.splitlines():
            fail("kernel configuration lost the built-in audio-profile group")
    for item in firmware:
        if not _file_contains(vmlinux, item.contents):
            fail(f"kernel artifact does not embed audio-profile input {item.destination}")

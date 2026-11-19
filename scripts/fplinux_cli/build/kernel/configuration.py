# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and validate the SoC, build-type and static firmware Kconfig inputs."""

from __future__ import annotations

import mmap
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.common import fail, replace_file_atomically
from fplinux_cli.manifests.kernel import compose_kernel_config, kconfig_values, kernel_config_paths
from fplinux_cli.workspace.capture import WorkspaceFile

if TYPE_CHECKING:
    from pathlib import Path


def prepare_kernel_config(
    target: str, target_config: dict[str, Any], platform: dict[str, Any], work: Path
) -> tuple[Path, tuple[Path, ...]]:
    """Write the effective configuration and retain its selected policy inputs."""
    config_paths = kernel_config_paths(target, target_config, platform)
    work.mkdir(parents=True, exist_ok=True)
    defconfig = work / "kernel.defconfig"
    boot_font = (
        None
        if platform["bootstrap"]["backend"] == "ums9117"
        else target_config["display"]["boot_font"]
    )
    defconfig.write_bytes(compose_kernel_config(*config_paths, boot_font=boot_font))
    return defconfig, config_paths


def assert_build_type_kconfig(config: Path, fragment: Path, build_type: str) -> None:
    """Require the selected type's capabilities after Kconfig dependency resolution."""
    requested = kconfig_values(inputs_build.require_file(fragment).read_text())
    actual = kconfig_values(inputs_build.require_file(config).read_text())
    for symbol, value in requested.items():
        if actual.get(symbol, "n") != value:
            fail(f"{build_type} build did not preserve {symbol}={value}")


@dataclass(frozen=True)
class BuiltinFirmware:
    """The exact selected files shared by Kconfig, causal receipts and verification."""

    directory: Path
    files: tuple[WorkspaceFile, ...]

    def kconfig_arguments(self) -> list[str]:
        """Set the complete selection once, including an explicitly empty selection."""
        return [
            "--set-str",
            "EXTRA_FIRMWARE",
            " ".join(item.path for item in self.files),
            "--set-str",
            "EXTRA_FIRMWARE_DIR",
            str(self.directory) if self.files else "",
        ]

    def implementation(self) -> list[tuple[str, Path]]:
        """Expose only selected materialized bytes to the existing Kbuild receipt."""
        return [
            (f"generated/builtin-firmware/{item.path}", self.directory / item.path)
            for item in self.files
        ]

    def verify(self, config_path: Path, vmlinux: Path) -> None:
        """Require the complete configured selection and every exact file in vmlinux."""
        values = kconfig_values(inputs_build.require_file(config_path).read_text())
        names = " ".join(item.path for item in self.files)
        directory = str(self.directory) if self.files else ""
        if (
            values.get("CONFIG_EXTRA_FIRMWARE", '""') != f'"{names}"'
            or values.get("CONFIG_EXTRA_FIRMWARE_DIR", '""') != f'"{directory}"'
        ):
            fail("kernel configuration lost its built-in firmware selection")
        for item in self.files:
            if not _file_contains(vmlinux, item.contents):
                fail(f"kernel artifact does not embed firmware input {item.path}")


def prepare_builtin_firmware(
    target_config: dict[str, Any],
    *,
    source: Path,
    work: Path,
) -> BuiltinFirmware:
    """Provide the shared kernel's static panel firmware."""
    files: dict[str, WorkspaceFile] = {}
    panel = target_config["display"]["panel"]
    if panel is not None and panel["renderer"] == "mipi-dbi":
        name = panel["firmware"]
        contents = inputs_build.require_file(source / "firmware" / name).read_bytes()
        files[name] = WorkspaceFile(name, contents, 0o644)
    directory = work / "builtin-firmware"
    selected = tuple(files[name] for name in sorted(files))
    if selected:
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        for firmware in selected:
            destination = directory / firmware.path
            destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            replace_file_atomically(destination, firmware.contents, firmware.mode, sync=False)
    return BuiltinFirmware(directory, selected)


def _file_contains(path: Path, contents: bytes) -> bool:
    """Search a built artifact without copying the complete file into host memory."""
    with (
        path.open("rb") as stream,
        mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image,
    ):
        return image.find(contents) >= 0

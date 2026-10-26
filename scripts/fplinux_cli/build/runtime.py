# SPDX-License-Identifier: GPL-2.0-only
"""Declare the executable source closure captured with each immutable bundle."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.values import relative_value

if TYPE_CHECKING:
    from collections.abc import Collection
    from pathlib import Path

_RUNTIME_MODULES = (
    "__init__.py",
    "common.py",
    "device_data/__init__.py",
    "device_data/formats.py",
    "manifests/__init__.py",
    "manifests/audio.py",
    "manifests/identity.py",
    "manifests/values.py",
    "runtime/__init__.py",
    "runtime/cli.py",
    "runtime/workflow.py",
    "runtime/bundle.py",
    "runtime/boot.py",
    "runtime/events.py",
    "runtime/ssh_transport.py",
    "runtime/windows_host.py",
    "runtime/nand_backup.py",
)
_PREPARATION_MODULES = (
    "device_data/audio_profile.py",
    "device_data/original_files.py",
    "device_data/partitions.py",
    "device_data/prepare.py",
)
_RADIO_MODULES = (
    "device_data/bluetooth_firmware.py",
    "device_data/cm4.py",
    "device_data/fitted.py",
    "device_data/fm_radio.py",
)


def device_data_descriptor(target: str, config: dict[str, Any]) -> dict[str, Any] | None:
    """Describe the selected RAM phone's required data without adding preparation policy."""
    declarations = config.get("device_data", {})
    groups = declarations.get("groups", {})
    if (
        config["platform"] != "ums9117"
        or config["linux"]["root"]["kind"] != "initramfs"
        or not groups
    ):
        return None
    parser = declarations.get("parser")
    nand = config.get("nand")
    return {
        "parser": (
            f"targets/{target}/{relative_value(parser, 'device-data parser')}"
            if parser is not None
            else None
        ),
        "groups": groups,
        "audio_policy": config["audio_policy"],
        "ecc_module": (
            f"platforms/{config['platform']}/host/nand_ecc.py"
            if nand is not None and nand.get("ecc_format")
            else None
        ),
    }


def runtime_source_files(
    target: str, config: dict[str, Any], *, root: Path | None = None
) -> dict[str, Path]:
    """Map delivered paths to their sole build-time source, preserving module ownership."""
    source_root = common.ROOT if root is None else root
    selected = list(_RUNTIME_MODULES)
    if config["platform"] == "ums9117":
        selected.extend(("device_data/cpio.py", "device_data/inputs.py"))
    descriptor = device_data_descriptor(target, config)
    if descriptor is not None:
        selected.extend(_PREPARATION_MODULES)
        if {"bluetooth", "fm-radio"} & descriptor["groups"].keys():
            selected.extend(_RADIO_MODULES)
        if {"bluetooth", "wcn-firmware"} & descriptor["groups"].keys():
            selected.append("device_data/running_nv.py")
    files = {
        f"scripts/fplinux_cli/{name}": source_root / "scripts/fplinux_cli" / name
        for name in selected
    }
    files["fplinux.py"] = source_root / "common/runtime.py"
    files["60-fplinux.rules"] = source_root / "common/60-fplinux.rules"
    for launcher in ("Run.sh", "Run.cmd"):
        files[launcher] = source_root / "common" / launcher
    adapter = f"platforms/{config['platform']}/host/adapter.py"
    files[adapter] = source_root / adapter
    for source in sorted((source_root / "common/windows").rglob("*.ps1")):
        files[source.relative_to(source_root).as_posix()] = source
    if descriptor is not None:
        for role in ("parser", "ecc_module"):
            relative = descriptor[role]
            if relative is not None:
                files[relative] = source_root / relative
    return files


def runtime_host_tools(platform: dict[str, Any], *, nand_ecc: bool) -> dict[str, str]:
    """Select the native tools required by the published runtime roles."""
    tools = dict(platform["host"]["runtime_tools"])
    if nand_ecc:
        from fplinux_cli.device_data.prepare import NAND_ECC_TOOL  # noqa: PLC0415

        tools["nand_ecc"] = NAND_ECC_TOOL
    return tools



def runtime_needs_nand_ecc(config: dict[str, Any]) -> bool:
    """Require native ECC only for the selected RAM data-preparation path."""
    return bool(
        config["linux"]["root"]["kind"] == "initramfs"
        and config.get("device_data", {}).get("groups")
        and (config.get("nand") or {}).get("ecc_format")
    )



def selected_host_tool_recipes(
    platform: dict[str, Any], tools: Collection[str]
) -> list[dict[str, Any]]:
    """Resolve every requested native tool before building or capturing its inputs."""
    required = set(tools)
    recipes = [recipe for recipe in platform["host"]["tools"] if recipe["name"] in required]
    missing = required - {recipe["name"] for recipe in recipes}
    if missing:
        fail(f"platform host tools are not declared: {', '.join(sorted(missing))}")
    return recipes

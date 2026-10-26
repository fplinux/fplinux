# SPDX-License-Identifier: GPL-2.0-only
"""Identify the selected physical acquisition and phone-data preparation behavior."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING, Any

from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    all_path_properties,
    parse_nul_string_list,
)
from fplinux_cli.build.runtime import preparation_source_files
from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes, sha256_file

if TYPE_CHECKING:
    from pathlib import Path

_NAND_SOURCES = (
    "drivers/memory/ums9117/ums9117-nandc-ro.c",
    "drivers/memory/ums9117/ums9117-nandc-chip.c",
    "drivers/memory/ums9117/ums9117-nandc-chip.h",
    "drivers/memory/ums9117/ums9117-nandc-feature.c",
    "drivers/memory/ums9117/ums9117-nandc-feature.h",
    "include/linux/soc/ums9117/mmio.h",
)
_BUS_PROPERTIES = {"#address-cells", "#size-cells", "ranges", "dma-ranges", "status"}
_EMMC_FILES = (
    "drivers/mmc/host/sdhci.c",
    "drivers/mmc/host/sdhci.h",
    "drivers/mmc/host/sdhci-pltfm.c",
    "drivers/mmc/host/sdhci-pltfm.h",
    "drivers/mmc/host/sdhci-sprd.c",
    "drivers/mmc/host/sdhci-sprd-emmc-ro.h",
    "drivers/clk/sprd/sc9820e-sdio-clock.c",
    "drivers/regulator/sc2721-regulator.c",
    "drivers/spi/spi-sprd-adi.c",
    "drivers/hwspinlock/sprd_hwspinlock.c",
    "block/partitions/efi.c",
)
_EMMC_DIRECTORIES = ("drivers/mmc/core", "include/linux/mmc", "fs/ext4")


def _emmc_tree(dtb: Path, raw_device: str) -> dict[str, dict[str, Any]]:
    """Retain the reader's effective buses, clocks and supplies with stable references."""
    nodes = all_path_properties(dtb)
    readers = [
        path
        for path, properties in nodes.items()
        if "compatible" in properties
        and "fplinux,sc9820e-emmc-ro"
        in parse_nul_string_list(properties["compatible"], f"{path} compatible")
    ]
    if len(readers) != 1:
        message = "eMMC acquisition requires exactly one compiled read-only reader node"
        raise DeviceTreeError(message)
    handles = {
        value: path
        for path, properties in nodes.items()
        for name, value in properties.items()
        if name in {"phandle", "linux,phandle"}
    }
    selected: dict[str, dict[str, Any]] = {}
    pending = [readers[0]]
    resource_paths = {readers[0]}

    def references(value: bytes, cells_property: str | None) -> list[dict[str, Any]]:
        if not value or len(value) % 4:
            message = "eMMC resource reference must contain complete cells"
            raise DeviceTreeError(message)
        cells = list(struct.unpack(f">{len(value) // 4}I", value))
        normalized = []
        while cells:
            handle = struct.pack(">I", cells.pop(0))
            provider = handles.get(handle)
            if provider is None:
                message = "eMMC resource does not identify a compiled provider"
                raise DeviceTreeError(message)
            count = 0
            if cells_property is not None:
                encoded = nodes[provider].get(cells_property, b"")
                if len(encoded) != 4:
                    raise DeviceTreeError(f"eMMC provider lacks {cells_property}: {provider}")
                count = struct.unpack(">I", encoded)[0]
            if len(cells) < count:
                message = "eMMC resource reference is truncated"
                raise DeviceTreeError(message)
            normalized.append({"provider": provider, "arguments": cells[:count]})
            del cells[:count]
            pending.append(provider)
            resource_paths.add(provider)
        return normalized

    while pending:
        path = pending.pop()
        if path in selected:
            continue
        properties = nodes[path]
        full_node = path in resource_paths or (path != "/" and "compatible" in properties)
        selected[path] = {}
        for name, value in properties.items():
            if name in {"phandle", "linux,phandle"}:
                continue
            if not full_node and name not in _BUS_PROPERTIES | {"interrupt-parent"}:
                continue
            if name in {"clocks", "hwlocks"}:
                cells_property = "#clock-cells" if name == "clocks" else "#hwlock-cells"
                selected[path][name] = references(value, cells_property)
            elif name.endswith("-supply") or "syscon" in name or name == "interrupt-parent":
                selected[path][name] = references(value, None)
            else:
                selected[path][name] = value.hex()
        if path != "/":
            pending.append(path.rpartition("/")[0] or "/")
    alias = f"mmc{raw_device.removeprefix('/dev/mmcblk')}"
    selected["/aliases"] = {alias: nodes.get("/aliases", {}).get(alias, b"").hex()}
    return selected


def _emmc_acquisition(
    config: dict[str, Any],
    *,
    source_root: Path,
    source_catalog_root: Path,
    linux_source: Path,
    dtb: Path,
) -> str:
    emmc = config["emmc"]
    try:
        tree = _emmc_tree(dtb, emmc["raw_device"])
    except DeviceTreeError as error:
        fail(f"eMMC acquisition device tree is invalid: {error}")
    kernel_paths = {relative: linux_source / relative for relative in _EMMC_FILES}
    for relative in _EMMC_DIRECTORIES:
        for source in sorted((linux_source / relative).rglob("*")):
            if source.is_file() and source.suffix in {".c", ".h"}:
                kernel_paths[source.relative_to(linux_source).as_posix()] = source
    host_sources = (
        "scripts/fplinux_cli/runtime/emmc_files.py",
        "scripts/fplinux_cli/runtime/emmc-files/collect.sh",
        "scripts/fplinux_cli/device_data/emmc.py",
        "scripts/fplinux_cli/device_data/emmc-files/read_files.py",
    )
    record = {
        "reader": emmc,
        "host": {relative: sha256_file(source_root / relative) for relative in host_sources},
        "kernel": {relative: sha256_file(source) for relative, source in kernel_paths.items()},
        "device_tree": tree,
        "python_reader": _emmc_python_inputs(source_catalog_root),
    }
    return sha256_bytes(canonical_json_bytes(record))


def _nand_tree(dtb: Path) -> dict[str, dict[str, str]]:
    """Select effective reader resources without unrelated nodes or numeric phandles."""
    nodes = all_path_properties(dtb)
    readers = [
        path
        for path, properties in nodes.items()
        if "compatible" in properties
        and "fplinux,ums9117-nandc-ro"
        in parse_nul_string_list(properties["compatible"], f"{path} compatible")
    ]
    if len(readers) != 1:
        message = "NAND acquisition requires exactly one compiled reader node"
        raise DeviceTreeError(message)
    reader = readers[0]
    reference = nodes[reader].get("sprd,aon-apb", b"")
    if len(reference) != 4:
        message = "NAND reader sprd,aon-apb must contain one phandle"
        raise DeviceTreeError(message)
    phandle = struct.unpack(">I", reference)[0]
    referenced = [
        path
        for path, properties in nodes.items()
        if any(properties.get(name) == reference for name in ("phandle", "linux,phandle"))
    ]
    if phandle == 0 or len(referenced) != 1:
        message = "NAND reader sprd,aon-apb does not identify one syscon"
        raise DeviceTreeError(message)
    syscon = referenced[0]
    selected: dict[str, dict[str, str]] = {}
    for path in (reader, syscon):
        selected[path] = {
            name: value.hex()
            for name, value in nodes[path].items()
            if name not in {"phandle", "linux,phandle", "sprd,aon-apb"}
        }
        ancestor = path.rpartition("/")[0] or "/"
        while True:
            selected.setdefault(
                ancestor,
                {
                    name: value.hex()
                    for name, value in nodes[ancestor].items()
                    if name in _BUS_PROPERTIES
                },
            )
            if ancestor == "/":
                break
            ancestor = ancestor.rpartition("/")[0] or "/"
    selected[reader]["sprd,aon-apb"] = syscon
    return selected


def acquisition_recipe(
    config: dict[str, Any],
    *,
    source_root: Path,
    linux_source: Path,
    dtb: Path,
    source_catalog_root: Path | None = None,
) -> str:
    """Bind raw backup reuse to the reader, physical contract and effective resources."""
    if config.get("emmc") is not None:
        return _emmc_acquisition(
            config,
            source_root=source_root,
            source_catalog_root=source_root
            if source_catalog_root is None
            else source_catalog_root,
            linux_source=linux_source,
            dtb=dtb,
        )
    nand = config.get("nand")
    if nand is None:
        fail("NAND acquisition requires a declared reader")
    host_sources = (
        "scripts/fplinux_cli/runtime/nand_backup.py",
        "scripts/fplinux_cli/device_data/nand_geometry.py",
    )
    try:
        tree = _nand_tree(dtb)
    except DeviceTreeError as error:
        fail(f"NAND acquisition device tree is invalid: {error}")
    record = {
        "reader": {
            key: nand[key] for key in ("raw_device", "id", "raw_page_bytes") if key in nand
        },
        "host": {relative: sha256_file(source_root / relative) for relative in host_sources},
        "kernel": {relative: sha256_file(linux_source / relative) for relative in _NAND_SOURCES},
        "device_tree": tree,
    }
    return sha256_bytes(canonical_json_bytes(record))


def _emmc_python_inputs(root: Path) -> dict[str, Any]:
    """Bind saved-image extraction to its pinned originals and pure-package projection."""
    from fplinux_cli.build.host_tools.phone_data import preparation_sources  # noqa: PLC0415

    return {
        "sources": {
            source.id: {
                "version": source.version,
                "algorithm": source.algorithm,
                "checksum": source.checksum,
            }
            for source in preparation_sources(root)
        },
        "projection": sha256_file(root / "scripts/fplinux_cli/build/host_tools/phone_data.py"),
    }


def preparation_recipe(
    target: str,
    config: dict[str, Any],
    *,
    source_root: Path,
    source_catalog_root: Path | None = None,
) -> str:
    """Identify delivered preparation inputs before selecting the host-native ECC tool."""
    groups = config.get("device_data", {}).get("groups", {})
    if not groups:
        fail("phone-data preparation requires declared device-data groups")
    paths = preparation_source_files(target, config, root=source_root)
    record = {
        "target": target,
        "machine_compatible": config["identity"]["compatible"],
        "groups": groups,
        "audio_policy": config["audio_policy"],
        "nand": config.get("nand"),
        "emmc": config.get("emmc"),
        "required_groups": config.get("device_data", {}).get("required_groups", list(groups)),
        "sources": {relative: sha256_file(source) for relative, source in paths.items()},
    }
    if config.get("emmc") is not None:
        catalog_root = source_root if source_catalog_root is None else source_catalog_root
        record["python_reader"] = _emmc_python_inputs(catalog_root)
    return sha256_bytes(canonical_json_bytes(record))

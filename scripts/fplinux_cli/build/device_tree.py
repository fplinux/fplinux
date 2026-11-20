# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- validation failures use exact local protocol diagnostics.
"""Project and verify properties of compiled flattened device trees."""

from __future__ import annotations

import shutil
import struct
from typing import TYPE_CHECKING, Any

from fplinux_cli.build.process import run
from fplinux_cli.build.storage.layout import root_bootargs
from fplinux_cli.fdt import FdtError, parse_fdt

if TYPE_CHECKING:
    from collections.abc import Iterable, Mapping, Sequence
    from pathlib import Path

    from fplinux_cli.fdt import FlattenedDeviceTree


class DeviceTreeError(ValueError):
    """A compiled device tree violates the expected binary contract."""


def _validate_path(path: str) -> None:
    """Require a canonical absolute path for exact lookup."""
    if path == "/":
        return
    if not path.startswith("/") or any(part in {"", ".", ".."} for part in path[1:].split("/")):
        raise ValueError(f"device-tree path is not canonical: {path!r}")


def _read_tree(tree: bytes | Path) -> bytes:
    """Return bytes from an in-memory tree or one compiled DTB path."""
    if isinstance(tree, bytes):
        return tree
    try:
        return tree.read_bytes()
    except OSError as error:
        raise DeviceTreeError(f"cannot read compiled DTB {tree}: {error}") from error


def _parse_tree(data: bytes) -> FlattenedDeviceTree:
    """Parse a compiled DTB, including any zero padding inside its structure block."""
    try:
        return parse_fdt(data, allow_structure_padding=True)
    except FdtError as error:
        raise DeviceTreeError(f"compiled DTB {error}") from error


def _property_values(data: bytes, ranges: dict[str, slice]) -> dict[str, bytes]:
    """Read only the requested node's values from their validated byte ranges."""
    return {name: data[location] for name, location in ranges.items()}


def exact_path_properties(
    tree: bytes | Path, paths: str | Iterable[str]
) -> dict[str, dict[str, bytes]]:
    """Return all properties belonging to the requested exact node paths."""
    data = _read_tree(tree)
    requested_paths = (paths,) if isinstance(paths, str) else tuple(paths)
    if not requested_paths:
        raise ValueError("at least one device-tree path is required")
    if len(requested_paths) != len(set(requested_paths)):
        raise ValueError("device-tree paths must not contain duplicates")
    for path in requested_paths:
        _validate_path(path)
    ranges = _parse_tree(data).property_ranges
    missing = set(requested_paths) - ranges.keys()
    if missing:
        raise DeviceTreeError(f"compiled DTB lacks node {min(missing)}")
    return {path: _property_values(data, ranges[path]) for path in requested_paths}


def all_path_properties(tree: bytes | Path) -> dict[str, dict[str, bytes]]:
    """Return every exact node and property after validating the complete tree."""
    data = _read_tree(tree)
    return {
        path: _property_values(data, ranges)
        for path, ranges in _parse_tree(data).property_ranges.items()
    }


def memory_reservations(tree: bytes | Path) -> tuple[tuple[int, int], ...]:
    """Return the validated FDT memory-reservation address/size pairs."""
    return _parse_tree(_read_tree(tree)).reservations


def parse_nul_string(value: bytes, name: str) -> str:
    """Decode one DT string with exactly one trailing NUL byte."""
    if not value or value[-1:] != b"\0" or b"\0" in value[:-1]:
        raise DeviceTreeError(f"{name} is not one NUL-terminated string")
    try:
        return value[:-1].decode("utf-8")
    except UnicodeDecodeError as error:
        raise DeviceTreeError(f"{name} is not valid UTF-8") from error


def parse_nul_string_list(value: bytes, name: str) -> tuple[str, ...]:
    """Decode a non-empty DT string-list with no empty elements."""
    if not value or value[-1:] != b"\0":
        raise DeviceTreeError(f"{name} is not a NUL-terminated string-list")
    encoded = value[:-1].split(b"\0")
    if not encoded or any(not item for item in encoded):
        raise DeviceTreeError(f"{name} contains an empty string-list element")
    try:
        return tuple(item.decode("utf-8") for item in encoded)
    except UnicodeDecodeError as error:
        raise DeviceTreeError(f"{name} is not valid UTF-8") from error


def verify_dtb_kconfig(
    tree: bytes | Path,
    config: Mapping[str, str],
    checks: Sequence[Mapping[str, str]],
) -> None:
    """Require built-in ownership and declared property support for enabled nodes."""
    if not checks:
        return
    owners: dict[str, list[Mapping[str, str]]] = {}
    for check in checks:
        owners.setdefault(check["path"], []).append(check)
    nodes = exact_path_properties(tree, owners)
    for path, requirements in owners.items():
        node = nodes[path]
        status = parse_nul_string(node.get("status", b"okay\0"), f"{path} status")
        if status not in {"ok", "okay"}:
            continue
        compatibles = parse_nul_string_list(node.get("compatible", b""), f"{path} compatible")
        matched = [check for check in requirements if check["compatible"] in compatibles]
        if not matched:
            raise DeviceTreeError(f"enabled DTB node {path} has no declared Kconfig owner")
        for check in matched:
            property_name = check.get("property")
            if property_name is not None and property_name not in node:
                continue
            compatible = check["compatible"]
            symbol = check["config"]
            if config.get(symbol, "n") != "y":
                condition = f" property {property_name}" if property_name is not None else ""
                raise DeviceTreeError(
                    f"enabled DTB node {path} compatible {compatible}{condition} "
                    f"requires {symbol}=y"
                )


def _u32_pair(value: bytes, name: str) -> tuple[int, int]:
    """Decode exactly one 32-bit address/size pair from a compiled property."""
    if len(value) != 8:
        raise DeviceTreeError(f"{name} is not one 32-bit address/size pair")
    return struct.unpack(">II", value)


def _layout_u32(layout: Mapping[str, int], field: str) -> int:
    """Read one already-normalized profile layout value defensively."""
    try:
        value = layout[field]
    except KeyError as error:
        raise DeviceTreeError(f"profile layout lacks {field}") from error
    if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
        raise DeviceTreeError(f"profile layout {field} is not a 32-bit integer")
    return value


def verify_profile_dtb_layout(
    tree: bytes | Path,
    layout: Mapping[str, int],
    linux_memory: Mapping[str, int],
) -> None:
    """Verify Linux RAM, the fixed FDT arena and the bootstrap display reservation."""
    ram_base = _layout_u32(layout, "ram_base")
    fdt_load = _layout_u32(layout, "fdt_load")
    fdt_limit = _layout_u32(layout, "fdt_size")
    fdt_pad_bytes = _layout_u32(layout, "fdt_pad")
    framebuffer = _layout_u32(layout, "framebuffer")
    framebuffer_size = _layout_u32(layout, "framebuffer_size")
    ram_size = _layout_u32(layout, "ram_size")
    linux_base = _layout_u32(linux_memory, "base")
    linux_size = _layout_u32(linux_memory, "size")
    if fdt_load % 0x1000:
        raise DeviceTreeError("profile DTB fixed FDT address is not page-aligned")
    if fdt_load < ram_base or fdt_load + fdt_limit > framebuffer:
        raise DeviceTreeError("profile DTB fixed FDT arena does not precede the framebuffer")
    if framebuffer + framebuffer_size != ram_base + ram_size:
        raise DeviceTreeError("profile DTB framebuffer does not end at the RAM boundary")
    if not ram_base <= linux_base < linux_base + linux_size <= fdt_load:
        raise DeviceTreeError(
            "profile DTB Linux memory overlaps reserved RAM or the fixed FDT arena"
        )

    memory_path = f"/memory@{linux_base:x}"
    reserved_path = f"/reserved-memory/framebuffer@{framebuffer:x}"
    tree_bytes = _read_tree(tree)
    properties = exact_path_properties(tree_bytes, (memory_path, reserved_path))
    memory = properties[memory_path]
    if parse_nul_string(memory.get("device_type", b""), f"{memory_path} device_type") != "memory":
        raise DeviceTreeError("profile DTB memory node is not memory")
    memory_base, memory_size = _u32_pair(memory.get("reg", b""), f"{memory_path} reg")
    if (memory_base, memory_size) != (linux_base, linux_size):
        raise DeviceTreeError("profile DTB memory range differs from the target Linux range")

    if len(tree_bytes) + fdt_pad_bytes > fdt_limit:
        raise DeviceTreeError("profile DTB plus U-Boot padding exceeds the fixed FDT arena")

    # The bootstrap scanout remains reserved while Linux allocates its own DMA buffers.
    reserved = properties[reserved_path]
    reserved_base, reserved_size = _u32_pair(reserved.get("reg", b""), f"{reserved_path} reg")
    if (reserved_base, reserved_size) != (framebuffer, framebuffer_size):
        raise DeviceTreeError("profile DTB framebuffer reservation differs from the layout")
    if reserved.get("no-map") != b"":
        raise DeviceTreeError("profile DTB framebuffer reservation must be no-map")


def verify_target_identity(
    tree: bytes | Path,
    target: str,
    model: str,
    compatibles: Sequence[str],
) -> None:
    """Require the compiled root identity to match the normalized target identity."""
    root = exact_path_properties(tree, "/")["/"]
    if "model" not in root:
        raise DeviceTreeError(f"{target} DTB root lacks property model")
    if "compatible" not in root:
        raise DeviceTreeError(f"{target} DTB root lacks property compatible")
    actual_model = parse_nul_string(root["model"], f"{target} DTB root model")
    actual_compatibles = parse_nul_string_list(root["compatible"], f"{target} DTB root compatible")
    expected_compatibles = tuple(compatibles)
    if actual_model != model:
        raise DeviceTreeError(
            f"{target} DTB model mismatch: expected {model!r}, got {actual_model!r}"
        )
    if actual_compatibles != expected_compatibles:
        raise DeviceTreeError(
            f"{target} DTB compatible mismatch: expected {expected_compatibles!r}, "
            f"got {actual_compatibles!r}"
        )


def write_boot_dtb(
    source: Path, output: Path, target_config: dict[str, Any], boot_recipe: str | None = None
) -> Path:
    """Apply the root profile and preserve board arguments in a copy of the DTB."""
    output.mkdir(parents=True, exist_ok=True)
    target = output / source.name
    shutil.copyfile(source, target)
    root = target_config["linux"]["root"]
    root_args = root_bootargs(root)
    bootargs = f"{root_args} fbcon=font:{target_config['display']['boot_font']}"
    chosen = exact_path_properties(source, "/chosen")["/chosen"]
    if board_args := chosen.get("bootargs"):
        default_args = root_bootargs({"kind": "initramfs"})
        flags = parse_nul_string(board_args, "board boot arguments")
        extra_args = flags.removeprefix(root_args).removeprefix(default_args).strip()
        if extra_args:
            bootargs += f" {extra_args}"
    run(["fdtput", "-t", "s", str(target), "/chosen", "bootargs", bootargs])
    if boot_recipe is not None:
        run(["fdtput", "-t", "s", str(target), "/chosen", "fplinux,boot-recipe", boot_recipe])
    if root["kind"] == "external" and target_config["platform"] == "ums9117":
        for name in ("linux,initrd-start", "linux,initrd-end"):
            if name in chosen:
                run(["fdtput", "-d", str(target), "/chosen", name])
    return target


def verify_root_bootargs(tree: bytes | Path, root: dict[str, object]) -> None:
    """Require compiled external-root boot parameters to match the profile contract."""
    if root.get("kind") == "initramfs":
        return
    if root.get("kind") != "external":
        raise DeviceTreeError("DTB root contract kind is invalid")
    chosen = exact_path_properties(tree, "/chosen")["/chosen"]
    if "bootargs" not in chosen:
        raise DeviceTreeError("external-root DTB /chosen lacks property bootargs")
    bootargs = parse_nul_string(chosen["bootargs"], "external-root DTB bootargs")
    tokens = bootargs.split()
    expected = {
        "root": f"root=PARTUUID={root['partuuid']}",
        "rootfstype": f"rootfstype={root['filesystem']}",
        "rootwait": f"rootwait={root['wait_seconds']}",
        "init": "init=/sbin/init",
    }
    for key, token in expected.items():
        matches = [value for value in tokens if value == token]
        conflicting = [value for value in tokens if value.startswith(f"{key}=") and value != token]
        if len(matches) != 1 or conflicting:
            raise DeviceTreeError(f"external-root DTB bootargs must contain exactly one {token}")
    if tokens.count("rw") != 1 or "ro" in tokens:
        raise DeviceTreeError("external-root DTB bootargs must select rw and must not select ro")
    if any(argument == "rootwait" or argument.startswith("rdinit=") for argument in tokens):
        raise DeviceTreeError(
            "external-root DTB bootargs must use bounded rootwait and must not use rdinit"
        )

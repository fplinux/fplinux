# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- parser failures use exact local protocol diagnostics.
"""Parse exact properties from compiled flattened device trees."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Iterable, Mapping, Sequence
    from pathlib import Path

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9
FDT_HEADER_SIZE = 40


class DeviceTreeError(ValueError):
    """A compiled device tree violates the expected binary contract."""


def _path_parts(path: str) -> tuple[str, ...]:
    """Convert one canonical absolute path into its FDT node-name tuple."""
    if path == "/":
        return ("",)
    if not path.startswith("/") or path.endswith("/") or "//" in path:
        raise ValueError(f"device-tree path is not canonical: {path!r}")
    parts = path[1:].split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise ValueError(f"device-tree path is not canonical: {path!r}")
    return ("", *parts)


def _display_path(parts: tuple[str, ...]) -> str:
    """Render an internal node-name tuple as one absolute path."""
    return "/" if parts == ("",) else "/" + "/".join(parts[1:])


def _read_tree(tree: bytes | Path) -> bytes:
    """Return bytes from an in-memory tree or one compiled DTB path."""
    if isinstance(tree, bytes):
        return tree
    try:
        return tree.read_bytes()
    except OSError as error:
        raise DeviceTreeError(f"cannot read compiled DTB {tree}: {error}") from error


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
    requested = {_path_parts(path): path for path in requested_paths}

    if len(data) < FDT_HEADER_SIZE:
        raise DeviceTreeError("compiled DTB does not have a complete FDT header")
    (
        magic,
        total_size,
        structure_offset,
        strings_offset,
        reserved_offset,
        version,
        last_compatible_version,
        _boot_cpu,
        strings_size,
        structure_size,
    ) = struct.unpack_from(">10I", data)
    if magic != FDT_MAGIC:
        raise DeviceTreeError("compiled DTB has invalid FDT magic")
    if total_size != len(data) or version < 17:
        raise DeviceTreeError("compiled DTB has an invalid FDT header")
    if last_compatible_version > 17 or last_compatible_version > version:
        raise DeviceTreeError("compiled DTB has an unsupported FDT compatibility version")

    structure_end = structure_offset + structure_size
    strings_end = strings_offset + strings_size
    if (
        structure_offset < FDT_HEADER_SIZE
        or structure_offset % 4
        or structure_end > len(data)
        or strings_offset < FDT_HEADER_SIZE
        or strings_end > len(data)
        or reserved_offset < FDT_HEADER_SIZE
        or reserved_offset % 8
    ):
        raise DeviceTreeError("compiled DTB has invalid block bounds")
    if not (structure_end <= strings_offset or strings_end <= structure_offset):
        raise DeviceTreeError("compiled DTB structure and strings blocks overlap")

    reserved_limit = min(structure_offset, strings_offset)
    position = reserved_offset
    while position + 16 <= reserved_limit:
        address, size = struct.unpack_from(">QQ", data, position)
        position += 16
        if address == 0 and size == 0:
            break
    else:
        raise DeviceTreeError("compiled DTB has an unterminated memory reservation block")

    properties: dict[tuple[str, ...], dict[str, bytes]] = {}
    all_properties: dict[tuple[str, ...], set[str]] = {}
    seen_nodes: set[tuple[str, ...]] = set()
    nodes_with_children: set[tuple[str, ...]] = set()
    stack: list[str] = []
    saw_root = False
    root_closed = False
    position = structure_offset
    while position + 4 <= structure_end:
        token = struct.unpack_from(">I", data, position)[0]
        position += 4
        if token == FDT_BEGIN_NODE:
            if root_closed:
                raise DeviceTreeError("compiled DTB contains a node after the root node")
            terminator = data.find(b"\0", position, structure_end)
            if terminator < 0:
                raise DeviceTreeError("compiled DTB has an unterminated node name")
            try:
                name = data[position:terminator].decode("ascii")
            except UnicodeDecodeError as error:
                raise DeviceTreeError("compiled DTB has a non-ASCII node name") from error
            if not stack:
                if saw_root or name:
                    raise DeviceTreeError("compiled DTB does not begin with an empty root node")
                saw_root = True
            elif not name or "/" in name:
                raise DeviceTreeError("compiled DTB has an invalid child node name")
            else:
                nodes_with_children.add(tuple(stack))
            stack.append(name)
            path = tuple(stack)
            if path in seen_nodes:
                raise DeviceTreeError(f"compiled DTB repeats node {_display_path(path)}")
            seen_nodes.add(path)
            all_properties[path] = set()
            if path in requested:
                properties[path] = {}
            position = (terminator + 4) & ~3
            if position > structure_end:
                raise DeviceTreeError("compiled DTB has a truncated node name")
        elif token == FDT_END_NODE:
            if not stack:
                raise DeviceTreeError("compiled DTB has an unmatched end-node token")
            stack.pop()
            if not stack:
                root_closed = True
        elif token == FDT_PROP:
            if not stack:
                raise DeviceTreeError("compiled DTB has a property outside a node")
            path = tuple(stack)
            if path in nodes_with_children:
                raise DeviceTreeError(
                    f"compiled DTB {_display_path(path)} has a property after a child node"
                )
            if position + 8 > structure_end:
                raise DeviceTreeError("compiled DTB has a truncated property header")
            value_size, name_offset = struct.unpack_from(">II", data, position)
            position += 8
            value_end = position + value_size
            if value_end > structure_end or name_offset >= strings_size:
                raise DeviceTreeError("compiled DTB has an invalid property")
            name_start = strings_offset + name_offset
            name_end = data.find(b"\0", name_start, strings_end)
            if name_end < 0:
                raise DeviceTreeError("compiled DTB has an unterminated property name")
            try:
                name = data[name_start:name_end].decode("ascii")
            except UnicodeDecodeError as error:
                raise DeviceTreeError("compiled DTB has a non-ASCII property name") from error
            if not name:
                raise DeviceTreeError("compiled DTB has an empty property name")
            names = all_properties[path]
            if name in names:
                raise DeviceTreeError(
                    f"compiled DTB {_display_path(path)} repeats property {name}"
                )
            names.add(name)
            if path in properties:
                properties[path][name] = data[position:value_end]
            position = (value_end + 3) & ~3
            if position > structure_end:
                raise DeviceTreeError("compiled DTB has a truncated property value")
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            if stack or not saw_root or not root_closed:
                raise DeviceTreeError("compiled DTB ends before the root node is closed")
            if any(data[position:structure_end]):
                raise DeviceTreeError("compiled DTB has data after its final structure token")
            missing = requested.keys() - properties.keys()
            if missing:
                path = min(missing)
                raise DeviceTreeError(f"compiled DTB lacks node {_display_path(path)}")
            return {path: properties[parts] for parts, path in requested.items()}
        else:
            raise DeviceTreeError(f"compiled DTB has unknown structure token {token}")
    raise DeviceTreeError("compiled DTB lacks its final structure token")


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
) -> None:
    """Verify the selected profile's volatile layout against its compiled DTB."""
    ram_base = _layout_u32(layout, "ram_base")
    fdt_load = _layout_u32(layout, "fdt_load")
    fdt_limit = _layout_u32(layout, "fdt_size")
    fdt_pad_bytes = _layout_u32(layout, "fdt_pad")
    framebuffer = _layout_u32(layout, "framebuffer")
    framebuffer_size = _layout_u32(layout, "framebuffer_size")
    ram_size = _layout_u32(layout, "ram_size")
    if fdt_load % 0x1000:
        raise DeviceTreeError("profile DTB fixed FDT address is not page-aligned")
    if fdt_load < ram_base or fdt_load + fdt_limit > framebuffer:
        raise DeviceTreeError("profile DTB fixed FDT arena does not precede the framebuffer")
    if framebuffer + framebuffer_size != ram_base + ram_size:
        raise DeviceTreeError("profile DTB framebuffer does not end at the RAM boundary")

    memory_path = f"/memory@{ram_base:x}"
    reserved_path = f"/reserved-memory/framebuffer@{framebuffer:x}"
    display_path = f"/soc/display@{framebuffer:x}"
    tree_bytes = _read_tree(tree)
    properties = exact_path_properties(tree_bytes, (memory_path, reserved_path, display_path))
    memory = properties[memory_path]
    if parse_nul_string(memory.get("device_type", b""), f"{memory_path} device_type") != "memory":
        raise DeviceTreeError("profile DTB memory node is not memory")
    memory_base, memory_size = _u32_pair(memory.get("reg", b""), f"{memory_path} reg")
    if memory_base != ram_base or memory_base + memory_size != fdt_load:
        raise DeviceTreeError("profile DTB memory range must end exactly at the fixed FDT arena")

    if len(tree_bytes) + fdt_pad_bytes > fdt_limit:
        raise DeviceTreeError("profile DTB plus U-Boot padding exceeds the fixed FDT arena")

    reserved = properties[reserved_path]
    reserved_base, reserved_size = _u32_pair(reserved.get("reg", b""), f"{reserved_path} reg")
    if (reserved_base, reserved_size) != (framebuffer, framebuffer_size):
        raise DeviceTreeError("profile DTB framebuffer reservation differs from the layout")
    if reserved.get("no-map") != b"":
        raise DeviceTreeError("profile DTB framebuffer reservation must be no-map")

    display = properties[display_path]
    display_reg = display.get("reg", b"")
    if len(display_reg) < 8 or len(display_reg) % 8:
        raise DeviceTreeError("profile DTB display reg has no complete 32-bit ranges")
    display_base, display_size = _u32_pair(display_reg[:8], f"{display_path} first reg")
    names = parse_nul_string_list(display.get("reg-names", b""), f"{display_path} reg-names")
    if not names or names[0] != "framebuffer":
        raise DeviceTreeError("profile DTB display does not name its first range framebuffer")
    if (display_base, display_size) != (framebuffer, framebuffer_size):
        raise DeviceTreeError("profile DTB display framebuffer range differs from the layout")


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

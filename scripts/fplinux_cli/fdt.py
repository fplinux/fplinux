# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- parser failures use exact local protocol diagnostics.
"""Read exact property value ranges and memory reservations from a binary FDT."""

from __future__ import annotations

import struct
from dataclasses import dataclass

_MAGIC = 0xD00DFEED
_BEGIN_NODE = 1
_END_NODE = 2
_PROP = 3
_NOP = 4
_END = 9
_HEADER_SIZE = 40


class FdtError(ValueError):
    """The data violates the binary FDT format."""


@dataclass(frozen=True)
class FlattenedDeviceTree:
    """A validated reserve map and byte slices indexed by exact node path and name."""

    reservations: tuple[tuple[int, int], ...]
    property_ranges: dict[str, dict[str, slice]]


def _read_reservations(data: bytes, start: int, limit: int) -> tuple[tuple[int, int], ...]:
    """Require a complete reservation list before the other blocks."""
    reservations: list[tuple[int, int]] = []
    while start + 16 <= limit:
        address, size = struct.unpack_from(">QQ", data, start)
        if address == size == 0:
            return tuple(reservations)
        reservations.append((address, size))
        start += 16
    raise FdtError("has an unterminated memory reservation block")


def _read_name(data: bytes, start: int, limit: int, field: str) -> tuple[str, int]:
    """Decode one bounded FDT name and return the position past its terminator."""
    terminator = data.find(b"\0", start, limit)
    if terminator < 0:
        raise FdtError(f"has an unterminated {field} name")
    try:
        return data[start:terminator].decode("ascii"), terminator + 1
    except UnicodeDecodeError as error:
        raise FdtError(f"has a non-ASCII {field} name") from error


def _property_ranges(
    data: bytes,
    structure: tuple[int, int],
    strings: tuple[int, int],
    *,
    allow_structure_padding: bool,
) -> dict[str, dict[str, slice]]:
    """Validate the full structure before returning value slices to the caller."""
    position, structure_end = structure
    strings_start, strings_end = strings
    properties: dict[str, dict[str, slice]] = {}
    nodes_with_children: set[str] = set()
    stack: list[str] = []
    while position + 4 <= structure_end:
        token = struct.unpack_from(">I", data, position)[0]
        position += 4
        if token == _BEGIN_NODE:
            name, position = _read_name(data, position, structure_end, "node")
            if stack:
                if not name or "/" in name:
                    raise FdtError("has an invalid child node name")
                parent = stack[-1]
                nodes_with_children.add(parent)
                path = f"{parent.removesuffix('/')}/{name}"
            else:
                if "/" in properties:
                    raise FdtError("contains a node after the root node")
                if name:
                    raise FdtError("does not begin with an empty root node")
                path = "/"
            if path in properties:
                raise FdtError(f"repeats node {path}")
            properties[path] = {}
            stack.append(path)
            position = (position + 3) & ~3
            if position > structure_end:
                raise FdtError("has a truncated node name")
        elif token == _END_NODE:
            if not stack:
                raise FdtError("has an unmatched end-node token")
            stack.pop()
        elif token == _PROP:
            if not stack:
                raise FdtError("has a property outside a node")
            path = stack[-1]
            if path in nodes_with_children:
                raise FdtError(f"{path} has a property after a child node")
            if position + 8 > structure_end:
                raise FdtError("has a truncated property header")
            value_size, name_offset = struct.unpack_from(">II", data, position)
            position += 8
            value_end = position + value_size
            name_start = strings_start + name_offset
            if value_end > structure_end or name_start >= strings_end:
                raise FdtError("property is outside its block")
            name, _ = _read_name(data, name_start, strings_end, "property")
            if not name:
                raise FdtError("has an empty property name")
            if name in properties[path]:
                raise FdtError(f"{path} repeats property {name}")
            properties[path][name] = slice(position, value_end)
            position = (value_end + 3) & ~3
            if position > structure_end:
                raise FdtError("has a truncated property value")
        elif token == _NOP:
            continue
        elif token == _END:
            if stack or "/" not in properties:
                raise FdtError("ends before the root node is closed")
            if position != structure_end and (
                not allow_structure_padding or any(data[position:structure_end])
            ):
                raise FdtError("has data after its final structure token")
            return properties
        else:
            raise FdtError(f"has unknown structure token {token}")
    raise FdtError("lacks its final structure token")


def parse_fdt(data: bytes, *, allow_structure_padding: bool) -> FlattenedDeviceTree:
    """Parse validated ranges, permitting only zero padding after FDT_END when requested."""
    if len(data) < _HEADER_SIZE:
        raise FdtError("does not have a complete FDT header")
    (
        magic,
        total_size,
        structure_start,
        strings_start,
        reserved_start,
        version,
        last_compatible_version,
        _boot_cpu,
        strings_size,
        structure_size,
    ) = struct.unpack_from(">10I", data)
    if magic != _MAGIC:
        raise FdtError("has invalid FDT magic")
    if total_size != len(data) or version < 17:
        raise FdtError("has an invalid FDT header")
    if last_compatible_version > 17:
        raise FdtError("has an unsupported FDT compatibility version")

    structure_end = structure_start + structure_size
    strings_end = strings_start + strings_size
    if (
        structure_start < _HEADER_SIZE
        or structure_start % 4
        or structure_end > total_size
        or strings_start < _HEADER_SIZE
        or strings_end > total_size
        or reserved_start < _HEADER_SIZE
        or reserved_start % 8
    ):
        raise FdtError("has invalid block bounds")
    if not (structure_end <= strings_start or strings_end <= structure_start):
        raise FdtError("structure and strings blocks overlap")

    reservations = _read_reservations(data, reserved_start, min(structure_start, strings_start))
    ranges = _property_ranges(
        data,
        (structure_start, structure_end),
        (strings_start, strings_end),
        allow_structure_padding=allow_structure_padding,
    )
    return FlattenedDeviceTree(reservations, ranges)

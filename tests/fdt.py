# SPDX-License-Identifier: GPL-2.0-only
"""Independent binary FDT fixtures for parser and image tests."""

from __future__ import annotations

import struct

type FdtProperties = list[tuple[str, bytes]]
type FdtNode = tuple[str, FdtProperties, list[FdtNode]]


def binary_tree(
    properties: FdtProperties,
    children: list[FdtNode] | None = None,
) -> bytes:
    """Construct a minimal binary FDT with root properties and nested children."""
    child_nodes = children or []

    def property_names_for(nodes: list[FdtNode]) -> list[str]:
        """Collect property names from one subtree in traversal order."""
        names: list[str] = []
        for _name, node_properties, node_children in nodes:
            names.extend(name for name, _value in node_properties)
            names.extend(property_names_for(node_children))
        return names

    property_names = list(
        dict.fromkeys([*(name for name, _value in properties), *property_names_for(child_nodes)])
    )
    strings = b""
    name_offsets: dict[str, int] = {}
    for name in property_names:
        name_offsets[name] = len(strings)
        strings += name.encode("ascii") + b"\0"

    def aligned(value: bytes) -> bytes:
        return value + b"\0" * (-len(value) % 4)

    def encoded_property(name: str, value: bytes) -> bytes:
        return struct.pack(">III", 3, len(value), name_offsets[name]) + aligned(value)

    def encoded_node(
        name: str, node_properties: FdtProperties, node_children: list[FdtNode]
    ) -> bytes:
        return (
            struct.pack(">I", 1)
            + aligned(name.encode("ascii") + b"\0")
            + b"".join(
                encoded_property(property_name, value) for property_name, value in node_properties
            )
            + b"".join(
                encoded_node(child_name, child_properties, grandchildren)
                for child_name, child_properties, grandchildren in node_children
            )
            + struct.pack(">I", 2)
        )

    structure = encoded_node("", properties, child_nodes) + struct.pack(">I", 9)

    reserved = b"\0" * 16
    structure_offset = 40 + len(reserved)
    strings_offset = structure_offset + len(structure)
    total_size = strings_offset + len(strings)
    header = struct.pack(
        ">10I",
        0xD00DFEED,
        total_size,
        structure_offset,
        strings_offset,
        40,
        17,
        16,
        0,
        len(strings),
        len(structure),
    )
    return header + reserved + structure + strings

# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral tests for compiled device-tree identity verification."""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.device_tree import (
    DeviceTreeError,
    exact_path_properties,
    parse_nul_string,
    parse_nul_string_list,
    verify_target_identity,
)


def _aligned(value: bytes) -> bytes:
    """Pad one structure-block field to its required four-byte boundary."""
    return value + b"\0" * (-len(value) % 4)


def _compiled_tree(
    properties: list[tuple[str, bytes]],
    children: list[tuple[str, list[tuple[str, bytes]]]] | None = None,
) -> bytes:
    """Construct a real minimal FDT containing root properties and child nodes."""
    child_nodes = children or []
    property_names = list(
        dict.fromkeys(
            name
            for name, _value in [
                *properties,
                *(item for _child, values in child_nodes for item in values),
            ]
        )
    )
    strings = b""
    name_offsets: dict[str, int] = {}
    for name in property_names:
        name_offsets[name] = len(strings)
        strings += name.encode("ascii") + b"\0"

    def encoded_property(name: str, value: bytes) -> bytes:
        return struct.pack(">III", 3, len(value), name_offsets[name]) + _aligned(value)

    structure = struct.pack(">I", 1) + _aligned(b"\0")
    for name, value in properties:
        structure += encoded_property(name, value)
    for child, values in child_nodes:
        structure += struct.pack(">I", 1) + _aligned(child.encode("ascii") + b"\0")
        for name, value in values:
            structure += encoded_property(name, value)
        structure += struct.pack(">I", 2)
    structure += struct.pack(">II", 2, 9)

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


class DeviceTreePropertyTests(unittest.TestCase):
    """Read properties from exact paths in compiled binary trees."""

    def test_exact_paths_do_not_mix_properties_from_different_nodes(self) -> None:
        """The same property name at root and child retains path ownership."""
        tree = _compiled_tree(
            [("compatible", b"vendor,board\0")],
            [("chosen", [("compatible", b"fplinux,session\0"), ("bootargs", b"x\0")])],
        )

        properties = exact_path_properties(tree, ("/", "/chosen"))

        self.assertEqual(properties["/"]["compatible"], b"vendor,board\0")
        self.assertEqual(properties["/chosen"]["compatible"], b"fplinux,session\0")
        self.assertEqual(properties["/chosen"]["bootargs"], b"x\0")

    def test_missing_exact_path_is_rejected(self) -> None:
        """A similarly named property cannot substitute for the requested node."""
        tree = _compiled_tree([("model", b"Demo\0")])

        with self.assertRaisesRegex(DeviceTreeError, r"lacks node /chosen"):
            exact_path_properties(tree, "/chosen")

    def test_nul_string_parsers_reject_ambiguous_encodings(self) -> None:
        """Missing terminators and empty string-list members are not accepted."""
        with self.assertRaisesRegex(DeviceTreeError, "one NUL-terminated string"):
            parse_nul_string(b"Demo\0extra\0", "model")
        with self.assertRaisesRegex(DeviceTreeError, "NUL-terminated string-list"):
            parse_nul_string_list(b"vendor,board", "compatible")
        with self.assertRaisesRegex(DeviceTreeError, "empty string-list element"):
            parse_nul_string_list(b"vendor,board\0\0", "compatible")


class TargetIdentityTests(unittest.TestCase):
    """Verify target identity against observable root properties in a DTB."""

    target = "demo-target"
    model = "Demo Phone (D-1)"
    compatibles = ("vendor,demo-phone", "vendor,demo-soc")

    def tree(
        self,
        *,
        model: bytes | None = b"Demo Phone (D-1)\0",
        compatible: bytes | None = b"vendor,demo-phone\0vendor,demo-soc\0",
    ) -> bytes:
        """Build a root identity while allowing individual properties to be omitted."""
        properties: list[tuple[str, bytes]] = []
        if model is not None:
            properties.append(("model", model))
        if compatible is not None:
            properties.append(("compatible", compatible))
        return _compiled_tree(properties)

    def test_matching_identity_is_accepted_from_a_compiled_path(self) -> None:
        """The verifier reads and accepts a matching compiled DTB artifact."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "target.dtb"
            path.write_bytes(self.tree())

            verify_target_identity(path, self.target, self.model, self.compatibles)

    def test_model_mismatch_reports_the_target_and_both_values(self) -> None:
        """A different hardware model fails even when compatibles still match."""
        with self.assertRaisesRegex(
            DeviceTreeError,
            r"demo-target DTB model mismatch: expected 'Demo Phone \(D-1\)', got 'Other'",
        ):
            verify_target_identity(
                self.tree(model=b"Other\0"),
                self.target,
                self.model,
                self.compatibles,
            )

    def test_compatible_order_is_part_of_the_identity(self) -> None:
        """SoC-first fallback ordering cannot pass a target-first contract."""
        with self.assertRaisesRegex(DeviceTreeError, "compatible mismatch"):
            verify_target_identity(
                self.tree(compatible=b"vendor,demo-soc\0vendor,demo-phone\0"),
                self.target,
                self.model,
                self.compatibles,
            )

    def test_required_identity_properties_cannot_be_omitted(self) -> None:
        """Neither model nor compatible may be inferred from another artifact."""
        for missing, tree in (
            ("model", self.tree(model=None)),
            ("compatible", self.tree(compatible=None)),
        ):
            with (
                self.subTest(missing=missing),
                self.assertRaisesRegex(DeviceTreeError, rf"root lacks property {missing}"),
            ):
                verify_target_identity(tree, self.target, self.model, self.compatibles)


if __name__ == "__main__":
    unittest.main()

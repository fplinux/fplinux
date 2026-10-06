# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data nand scenarios."""

from __future__ import annotations

import unittest

from fplinux_cli.device_data import formats as device_data


class PhysicalPageFormatTests(unittest.TestCase):
    """Keep the physical main/OOB format separate from image compatibility."""

    def test_main_read_crosses_pages_without_copying_oob(self) -> None:
        """Both supported geometries exclude spare bytes from cross-page reads."""
        for spare in (64, 128):
            with self.subTest(spare=spare):
                raw = b"A" * 2048 + b"O" * spare + b"B" * 2048 + b"P" * spare
                nand = device_data.PhysicalNand(raw, 2048 + spare)

                self.assertEqual(nand.main_bytes(2044, 12), b"AAAA" + b"B" * 8)
                self.assertEqual(nand.main_bytes(0, 4096), b"A" * 2048 + b"B" * 2048)
                with self.assertRaisesRegex(ValueError, "outside"):
                    nand.main_bytes(4090, 7)

    def test_either_factory_marker_rejects_a_selected_block(self) -> None:
        """Neither factory marker may be ignored for a consumed block."""
        for page_bytes in (2112, 2176):
            unmarked = b"\xff" * (64 * page_bytes)
            device_data.PhysicalNand(unmarked, page_bytes).require_good_blocks(0, 2048)
            for marker in (2048, page_bytes + 2048):
                with self.subTest(page_bytes=page_bytes, marker=marker):
                    marked = bytearray(unmarked)
                    marked[marker] = 0
                    with self.assertRaisesRegex(ValueError, "block 0 is marked bad"):
                        device_data.PhysicalNand(bytes(marked), page_bytes).require_good_blocks(
                            0, 2048
                        )

    def test_complete_dump_size_is_validated_by_physical_geometry(self) -> None:
        """A fragment is rejected before any target partition policy interprets it."""
        for page_bytes, expected_size in ((2112, 138412032), (2176, 142606336)):
            for raw in (b"", b"\xff" * 2176, b"\xff" * 2112):
                with (
                    self.subTest(page_bytes=page_bytes, size=len(raw)),
                    self.assertRaisesRegex(
                        ValueError, f"complete {expected_size}-byte physical NAND backup"
                    ),
                ):
                    device_data.PhysicalNand.from_dump(raw, page_bytes=page_bytes)


if __name__ == "__main__":
    unittest.main()

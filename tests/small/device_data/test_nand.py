# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data nand scenarios."""

from __future__ import annotations

import pytest
from fplinux_cli.device_data import formats as device_data


class PhysicalPageFormatTests:
    """Keep the physical main/OOB format separate from image compatibility."""

    @pytest.mark.parametrize(
        "spare", [pytest.param(64, id="64-byte-oob"), pytest.param(128, id="128-byte-oob")]
    )
    def test_main_read_crosses_pages_without_copying_oob(self, spare: int) -> None:
        """Both supported geometries exclude spare bytes from cross-page reads."""
        raw = b"A" * 2048 + b"O" * spare + b"B" * 2048 + b"P" * spare
        nand = device_data.PhysicalNand(raw, 2048 + spare)

        assert nand.main_bytes(2044, 12) == b"AAAA" + b"B" * 8
        assert nand.main_bytes(0, 4096) == b"A" * 2048 + b"B" * 2048
        with pytest.raises(ValueError, match="outside"):
            nand.main_bytes(4090, 7)

    @pytest.mark.parametrize(
        ("page_bytes", "marker"),
        [
            pytest.param(2112, 2048, id="64-oob-first-page"),
            pytest.param(2112, 4160, id="64-oob-second-page"),
            pytest.param(2176, 2048, id="128-oob-first-page"),
            pytest.param(2176, 4224, id="128-oob-second-page"),
        ],
    )
    def test_either_factory_marker_rejects_a_selected_block(
        self, page_bytes: int, marker: int
    ) -> None:
        """Neither factory marker may be ignored for a consumed block."""
        unmarked = b"\xff" * (64 * page_bytes)
        device_data.PhysicalNand(unmarked, page_bytes).require_good_blocks(0, 2048)
        marked = bytearray(unmarked)
        marked[marker] = 0
        with pytest.raises(ValueError, match="block 0 is marked bad"):
            device_data.PhysicalNand(bytes(marked), page_bytes).require_good_blocks(0, 2048)

    @pytest.mark.parametrize(
        ("page_bytes", "expected_size"),
        [
            pytest.param(2112, 138412032, id="64-byte-oob"),
            pytest.param(2176, 142606336, id="128-byte-oob"),
        ],
    )
    @pytest.mark.parametrize(
        "raw",
        [
            pytest.param(b"", id="empty"),
            pytest.param(b"\xff" * 2176, id="128-oob-fragment"),
            pytest.param(b"\xff" * 2112, id="64-oob-fragment"),
        ],
    )
    def test_complete_dump_size_is_validated_by_physical_geometry(
        self, page_bytes: int, expected_size: int, raw: bytes
    ) -> None:
        """A fragment is rejected before any target partition policy interprets it."""
        with pytest.raises(
            ValueError, match=f"complete {expected_size}-byte physical NAND backup"
        ):
            device_data.PhysicalNand.from_dump(raw, page_bytes=page_bytes)

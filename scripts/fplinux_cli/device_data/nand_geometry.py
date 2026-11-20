# SPDX-License-Identifier: GPL-2.0-only
"""Physical NAND geometry shared by acquisition and interpretation."""

from __future__ import annotations

import re
from dataclasses import dataclass

from fplinux_cli.common import fail

PAGE_MAIN_BYTES = 2048
PHYSICAL_PAGE_COUNT = 65536
PAGES_PER_BLOCK = 64
BLOCK_MAIN_BYTES = PAGE_MAIN_BYTES * PAGES_PER_BLOCK
GEOMETRY_SIZE_KEYS = (
    "page_main_bytes",
    "oob_bytes",
    "pages_per_block",
    "block_count",
    "raw_bytes",
)
_ID_BYTES = re.compile(r"(?:[0-9a-f]{2})+")


@dataclass(frozen=True)
class NandGeometry:
    """Chip identity and physical layout reported by the running NAND reader."""

    id_bytes: str
    chip: str
    page_main_bytes: int
    oob_bytes: int
    pages_per_block: int
    block_count: int
    raw_bytes: int

    @property
    def raw_page_bytes(self) -> int:
        """Return the stored bytes per page: main area followed by OOB."""
        return self.page_main_bytes + self.oob_bytes


def require_consistent_geometry(geometry: NandGeometry, source: str) -> NandGeometry:
    """Require an identified chip whose total size matches its page layout."""
    if _ID_BYTES.fullmatch(geometry.id_bytes) is None:
        fail(f"{source} has invalid id_bytes: {geometry.id_bytes!r}")
    if not geometry.chip:
        fail(f"{source} has an empty chip name")
    layout = (
        geometry.page_main_bytes,
        geometry.oob_bytes,
        geometry.pages_per_block,
        geometry.block_count,
    )
    expected_raw_bytes = geometry.raw_page_bytes * geometry.pages_per_block * geometry.block_count
    if min(layout) < 1 or geometry.raw_bytes != expected_raw_bytes:
        fail(
            f"{source} is inconsistent: {geometry.page_main_bytes}+{geometry.oob_bytes}-byte "
            f"pages, {geometry.pages_per_block} pages per block, {geometry.block_count} blocks, "
            f"{geometry.raw_bytes} bytes"
        )
    return geometry


def require_declared_chip(
    geometry: NandGeometry,
    *,
    target: str,
    declared_id: int | None,
    declared_raw_page_bytes: int | None,
    source: str,
) -> None:
    """Reject a reported chip that contradicts the target's optional declared expectation."""
    if declared_id is None or declared_raw_page_bytes is None:
        return
    # The declared ID holds the manufacturer byte in bits 7:0 and the device byte in 15:8.
    declared_id_bytes = declared_id.to_bytes(2, "little").hex()
    if (geometry.id_bytes, geometry.raw_page_bytes) != (
        declared_id_bytes,
        declared_raw_page_bytes,
    ):
        fail(
            f"{source} does not match target {target}: reported id_bytes={geometry.id_bytes} "
            f"with {geometry.raw_page_bytes}-byte pages; target declares "
            f"id_bytes={declared_id_bytes} with {declared_raw_page_bytes}-byte pages"
        )


def family_page_bytes(geometry: NandGeometry) -> int:
    """Return the physical page size of a geometry that device data can interpret."""
    layout = (
        geometry.page_main_bytes,
        geometry.pages_per_block,
        geometry.pages_per_block * geometry.block_count,
    )
    if layout != (PAGE_MAIN_BYTES, PAGES_PER_BLOCK, PHYSICAL_PAGE_COUNT):
        raise ValueError(
            f"unsupported NAND geometry: {geometry.page_main_bytes}-byte main pages, "
            f"{geometry.pages_per_block} pages per block, {geometry.block_count} blocks; "
            f"expected {PAGE_MAIN_BYTES}-byte main pages, {PAGES_PER_BLOCK} pages per block "
            f"and {PHYSICAL_PAGE_COUNT} pages"
        )
    return geometry.raw_page_bytes

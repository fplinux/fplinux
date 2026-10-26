# SPDX-License-Identifier: GPL-2.0-only
"""Physical NAND geometry shared by acquisition and interpretation."""

from __future__ import annotations

from dataclasses import dataclass

PAGE_MAIN_BYTES = 2048
PHYSICAL_PAGE_COUNT = 65536
PAGES_PER_BLOCK = 64
BLOCK_MAIN_BYTES = PAGE_MAIN_BYTES * PAGES_PER_BLOCK


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

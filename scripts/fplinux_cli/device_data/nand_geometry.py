# SPDX-License-Identifier: GPL-2.0-only
"""Physical NAND geometry shared by acquisition and interpretation."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import TYPE_CHECKING

from fplinux_cli.common import fail

if TYPE_CHECKING:
    from collections.abc import Mapping
    from typing import Any, Literal

PAGE_MAIN_BYTES = 2048
PHYSICAL_PAGE_COUNT = 65536
PAGES_PER_BLOCK = 64
BLOCK_MAIN_BYTES = PAGE_MAIN_BYTES * PAGES_PER_BLOCK
_BCH12_MAIN_BYTES = {1088: 1024, 2176: 2048}
_LAYOUT_FIELDS = (
    "page_main_bytes",
    "oob_bytes",
    "pages_per_block",
    "block_count",
)
GEOMETRY_SIZE_KEYS = (*_LAYOUT_FIELDS, "raw_bytes")
NAND_ACQUISITION_FIELDS = ("raw_device", "interface", "id", "raw_page_bytes", *_LAYOUT_FIELDS)
_ID_BYTES = re.compile(r"(?:[0-9a-f]{2})+")


def validate_nand_declaration(nand: Mapping[str, Any], *, platform: str, name: str) -> None:
    """Check the shared NAND fields admitted by target and runtime manifests."""
    fields = set(nand)
    allowed = {*NAND_ACQUISITION_FIELDS, "ecc_format"}
    if "raw_device" not in fields or fields - allowed:
        raise ValueError(f"{name} requires raw_device and accepts only declared NAND fields")
    device = nand["raw_device"]
    if (
        not isinstance(device, str)
        or re.fullmatch(r"/dev/[A-Za-z0-9][A-Za-z0-9._-]*", device) is None
    ):
        raise ValueError(f"{name} raw_device must name one device directly under /dev")
    interface = nand.get("interface", "spi")
    if interface not in ("spi", "parallel"):
        raise ValueError(f"{name} interface must be spi or parallel")
    chip_fields = {"id", "raw_page_bytes"}
    if (fields & {"id", "raw_page_bytes", "ecc_format"} or interface == "parallel") and (
        not chip_fields <= fields
    ):
        raise ValueError(f"{name} requires both id and raw_page_bytes for a declared chip")
    layout_fields = set(_LAYOUT_FIELDS)
    if interface == "parallel" and not layout_fields <= fields:
        raise ValueError(f"{name} parallel interface requires a complete NAND layout")
    if interface == "spi" and fields & layout_fields:
        raise ValueError(f"{name} full NAND layout requires the parallel interface")

    size_fields = ("raw_page_bytes", *_LAYOUT_FIELDS)
    for key in ("id", *size_fields):
        if key not in nand:
            continue
        minimum, maximum = (0, 0xFFFF) if key == "id" else (1, 0xFFFFFFFF)
        value = nand[key]
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f"{name} {key} must be an integer in {minimum}..{maximum}")
    if interface == "parallel" and nand["raw_page_bytes"] != (
        nand["page_main_bytes"] + nand["oob_bytes"]
    ):
        raise ValueError(f"{name} raw_page_bytes must equal page_main_bytes plus oob_bytes")
    if "ecc_format" in fields:
        main_bytes = _BCH12_MAIN_BYTES.get(nand["raw_page_bytes"])
        if nand["ecc_format"] != "ums9117-bch12" or platform != "ums9117" or main_bytes is None:
            raise ValueError(
                f"{name} ECC requires ums9117-bch12 with 1088- or 2176-byte UMS9117 pages"
            )
        if nand.get("page_main_bytes", PAGE_MAIN_BYTES) != main_bytes:
            raise ValueError(f"{name} page_main_bytes disagrees with its ums9117-bch12 format")


def nand_interface(nand: Mapping[str, Any] | None) -> Literal["spi", "parallel"]:
    """Use the existing SPI contract when no interface is declared."""
    return "parallel" if nand is not None and nand.get("interface") == "parallel" else "spi"


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


def require_declared_geometry(
    geometry: NandGeometry,
    *,
    target: str,
    nand: Mapping[str, Any] | None,
    source: str,
) -> None:
    """Reject observed NAND geometry that contradicts the selected declaration."""
    if nand is None or "id" not in nand:
        return
    # The declared ID holds the manufacturer byte in bits 7:0 and the device byte in 15:8.
    declared_id_bytes = nand["id"].to_bytes(2, "little").hex()
    declared_raw_page_bytes = nand["raw_page_bytes"]
    if (geometry.id_bytes, geometry.raw_page_bytes) != (
        declared_id_bytes,
        declared_raw_page_bytes,
    ):
        fail(
            f"{source} does not match target {target}: reported id_bytes={geometry.id_bytes} "
            f"with {geometry.raw_page_bytes}-byte pages; target declares "
            f"id_bytes={declared_id_bytes} with {declared_raw_page_bytes}-byte pages"
        )
    if nand_interface(nand) == "parallel" and any(
        getattr(geometry, key) != nand[key] for key in _LAYOUT_FIELDS
    ):
        fail(
            f"{source} has unsupported NAND geometry for {target}: "
            f"target declares {nand['page_main_bytes']} main + {nand['oob_bytes']} OOB bytes "
            f"per page, {nand['pages_per_block']} pages per block and {nand['block_count']} blocks"
        )


def require_spi_dump_geometry(geometry: NandGeometry) -> None:
    """Require the legacy physical layout supported by SPI device-data parsers."""
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

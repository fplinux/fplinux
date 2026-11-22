# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Correct the selected main-area sectors of the declared UMS9117 BCH12 format."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path

CORRECTION_BITS = 12
_TOOL_SECTOR_BYTES = 1024
_RAW_PAGE_SECTOR_BYTES = {1088: 512, 2176: 1024}
_OOB_RECORD_BYTES = 32
_PARITY_OFFSET = 4
_PARITY_BYTES = 21
_TOOL = "ums9117-nand-ecc"


def decode_main(
    raw: bytes,
    page_bytes: int,
    offset: int,
    size: int,
    *,
    host_tools: Path,
) -> bytes:
    """Correct complete selected codewords and return only the requested main bytes.

    Factory markers and other OOB bytes are excluded from the BCH input.
    """
    sector_bytes = _RAW_PAGE_SECTOR_BYTES.get(page_bytes)
    if sector_bytes is None:
        message = "UMS9117 BCH12 requires 1088- or 2176-byte raw pages"
        raise ValueError(message)
    tool = host_tools / (_TOOL + ".exe" if os.name == "nt" else _TOOL)
    if tool.is_symlink() or not tool.is_file():
        raise ValueError(f"NAND ECC host tool is missing or invalid: {tool}")
    first_sector = offset // sector_bytes
    last_sector = (offset + size - 1) // sector_bytes
    padding_size = _TOOL_SECTOR_BYTES - sector_bytes
    erased_main = b"\xff" * sector_bytes
    erased_parity = b"\xff" * _PARITY_BYTES
    zero_padding = bytes(padding_size)
    erased_padding = b"\xff" * padding_size
    records = bytearray()
    for sector in range(first_sector, last_sector + 1):
        page, half = divmod(sector, 2)
        position = page * page_bytes + half * sector_bytes
        parity_offset = (
            page * page_bytes + 2 * sector_bytes + half * _OOB_RECORD_BYTES + _PARITY_OFFSET
        )
        data = raw[position : position + sector_bytes]
        parity = raw[parity_offset : parity_offset + _PARITY_BYTES]
        records.extend(data)
        if padding_size:
            # The tool reverses the data bytes, placing padding before the shorter codeword.
            padding = (
                erased_padding if data == erased_main and parity == erased_parity else zero_padding
            )
            records.extend(padding)
        records.extend(parity)
    try:
        result = subprocess.run(
            [str(tool)], input=bytes(records), capture_output=True, check=False
        )
    except OSError as error:
        raise ValueError(f"NAND ECC host tool could not run: {error}") from error
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(
            f"NAND ECC correction at main offset {first_sector * sector_bytes:#x}: {detail}"
        )
    expected_size = (last_sector - first_sector + 1) * _TOOL_SECTOR_BYTES
    if len(result.stdout) != expected_size:
        message = "NAND ECC host tool returned an incomplete main-area projection"
        raise ValueError(message)
    corrected = result.stdout
    if padding_size:
        compact = bytearray()
        for index in range(last_sector - first_sector + 1):
            start = index * _TOOL_SECTOR_BYTES
            record_start = index * (_TOOL_SECTOR_BYTES + _PARITY_BYTES)
            if (
                corrected[start + sector_bytes : start + _TOOL_SECTOR_BYTES]
                != records[record_start + sector_bytes : record_start + _TOOL_SECTOR_BYTES]
            ):
                sector_offset = (first_sector + index) * sector_bytes
                raise ValueError(
                    f"NAND ECC correction reached codeword padding at {sector_offset:#x}"
                )
            compact.extend(corrected[start : start + sector_bytes])
        corrected = bytes(compact)
    column = offset % sector_bytes
    return corrected[column : column + size]

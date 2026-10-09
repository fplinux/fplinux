# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Correct the selected main-area sectors of the declared UMS9117 BCH12 format."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path

_SECTOR_BYTES = 1024
_PAGE_MAIN_BYTES = 2048
_RAW_PAGE_BYTES = 2176
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
    if page_bytes != _RAW_PAGE_BYTES:
        message = "UMS9117 BCH12 requires 2048-byte main pages with 128-byte OOB"
        raise ValueError(message)
    tool = host_tools / (_TOOL + ".exe" if os.name == "nt" else _TOOL)
    if tool.is_symlink() or not tool.is_file():
        raise ValueError(f"NAND ECC host tool is missing or invalid: {tool}")
    first_sector = offset // _SECTOR_BYTES
    last_sector = (offset + size - 1) // _SECTOR_BYTES
    records = bytearray()
    for sector in range(first_sector, last_sector + 1):
        page, half = divmod(sector, 2)
        position = page * page_bytes + half * _SECTOR_BYTES
        parity = page * page_bytes + _PAGE_MAIN_BYTES + half * _OOB_RECORD_BYTES + _PARITY_OFFSET
        records.extend(raw[position : position + _SECTOR_BYTES])
        records.extend(raw[parity : parity + _PARITY_BYTES])
    try:
        result = subprocess.run(
            [str(tool)], input=bytes(records), capture_output=True, check=False
        )
    except OSError as error:
        raise ValueError(f"NAND ECC host tool could not run: {error}") from error
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(
            f"NAND ECC correction at main offset {first_sector * _SECTOR_BYTES:#x}: {detail}"
        )
    expected_size = (last_sector - first_sector + 1) * _SECTOR_BYTES
    if len(result.stdout) != expected_size:
        message = "NAND ECC host tool returned an incomplete main-area projection"
        raise ValueError(message)
    column = offset % _SECTOR_BYTES
    return result.stdout[column : column + size]

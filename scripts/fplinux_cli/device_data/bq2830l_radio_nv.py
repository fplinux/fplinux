# SPDX-License-Identifier: GPL-2.0-only
"""Read current short NV records through the BQ external-radio storage format."""

from __future__ import annotations

import struct
from collections import defaultdict

from fplinux_cli.device_data.formats import (
    BLOCK_MAIN_BYTES,
    PAGE_MAIN_BYTES,
    PAGES_PER_BLOCK,
    PhysicalNand,
    nv_checksum,
)

_SECTOR_BYTES = 1024
_SPARE_BYTES = 32
_SECTORS_PER_PAGE = 2
_SECTORS_PER_VIRTUAL_BLOCK = (PAGES_PER_BLOCK - 1) * _SECTORS_PER_PAGE
_VOLUME_HEADER = struct.Struct("<IIHHHHHHBB")
_INDEX = struct.Struct("<HHIII")
_RECORD = struct.Struct("<HHHHI")


def _gf_powers() -> tuple[int, ...]:
    powers: list[int] = []
    value = 1
    for _ in range(63):
        powers.append(value)
        value <<= 1
        if value & 64:
            value ^= 0x43
    return tuple(powers)


_GF_POWERS = _gf_powers()


def _corrections() -> dict[tuple[int, int], int]:
    corrections: dict[tuple[int, int], int] = {}
    for first in range(44):
        first_syndrome = _GF_POWERS[first]
        third_syndrome = _GF_POWERS[first * 3 % 63]
        corrections[first_syndrome, third_syndrome] = 1 << first
        for second in range(first + 1, 44):
            key = (
                first_syndrome ^ _GF_POWERS[second],
                third_syndrome ^ _GF_POWERS[second * 3 % 63],
            )
            corrections[key] = (1 << first) | (1 << second)
    return corrections


_CORRECTIONS = _corrections()


def _metadata(spare: bytes, virtual_blocks: int) -> tuple[int, int, int] | None:
    free = spare[3:4] + spare[25:32]
    if free[:4] == b"\xff" * 4:
        return None
    parity = (spare[1] << 4) | (spare[2] >> 4)
    codeword = parity | (int.from_bytes(free[:4], "big") << 12)
    first = third = 0
    for bit in range(44):
        if codeword & (1 << bit):
            first ^= _GF_POWERS[bit]
            third ^= _GF_POWERS[bit * 3 % 63]
    if first or third:
        correction = _CORRECTIONS.get((first, third))
        if correction is None:
            message = "damaged RunningNV address metadata"
            raise ValueError(message)
        codeword ^= correction
        free = (codeword >> 12).to_bytes(4, "big") + free[4:]
    packed = int.from_bytes(free[:4], "little")
    generation = packed & 0x7F
    logical = (packed >> 7) & 0xFFFFFF
    virtual_block, sector = divmod(logical, 2048)
    if (
        generation == 127
        or virtual_block >= virtual_blocks
        or sector >= _SECTORS_PER_VIRTUAL_BLOCK
    ):
        return None
    return generation, virtual_block, sector


def _require_empty_remapping(nand: PhysicalNand) -> None:
    tail: list[int] = []
    for block in range(nand.block_count - 1, -1, -1):
        marker = nand.raw[block * PAGES_PER_BLOCK * nand.page_bytes + PAGE_MAIN_BYTES]
        if marker.bit_count() > 5:
            tail.append(block)
            if len(tail) == 4:
                break
    if len(tail) != 4:
        message = "missing redundant NAND mapping records"
        raise ValueError(message)
    maps = [nand.main_bytes(block * BLOCK_MAIN_BYTES, 12) for block in tail[2:]]
    for header in maps:
        magic, version, _generation, count = struct.unpack("<4sIHH", header)
        if magic != b"_MAP" or version != 0x102 or count:
            message = "unsupported NAND remapping for current radio records"
            raise ValueError(message)
    if maps[0] != maps[1]:
        message = "ambiguous redundant NAND mapping records"
        raise ValueError(message)


def _sector_map(
    nand: PhysicalNand, extent: tuple[int, int], virtual_blocks: int
) -> dict[int, int]:
    start, size = extent
    if start % BLOCK_MAIN_BYTES or size % BLOCK_MAIN_BYTES:
        message = "RunningNV extent does not contain complete erase blocks"
        raise ValueError(message)
    nand.require_good_blocks(start, size)
    chains: dict[int, list[tuple[int, list[tuple[int, int]]]]] = defaultdict(list)
    for block in range(size // BLOCK_MAIN_BYTES):
        identity: tuple[int, int] | None = None
        sectors: list[tuple[int, int]] = []
        for page in range(1, PAGES_PER_BLOCK):
            physical_page = start // PAGE_MAIN_BYTES + block * PAGES_PER_BLOCK + page
            for sector in range(_SECTORS_PER_PAGE):
                spare_start = (
                    physical_page * nand.page_bytes + PAGE_MAIN_BYTES + sector * _SPARE_BYTES
                )
                tag = _metadata(nand.raw[spare_start : spare_start + _SPARE_BYTES], virtual_blocks)
                if tag is None:
                    continue
                generation, virtual_block, virtual_sector = tag
                if identity is None:
                    identity = (generation, virtual_block)
                elif identity != (generation, virtual_block):
                    message = "inconsistent RunningNV block metadata"
                    raise ValueError(message)
                physical = (
                    start
                    + block * BLOCK_MAIN_BYTES
                    + page * PAGE_MAIN_BYTES
                    + sector * _SECTOR_BYTES
                )
                sectors.append((virtual_sector, physical))
        if identity is not None:
            generation, virtual_block = identity
            chains[virtual_block].append((generation, sectors))
    mapping: dict[int, int] = {}
    for virtual_block, chain in chains.items():
        if len({generation for generation, _ in chain}) != len(chain):
            message = "ambiguous RunningNV block generations"
            raise ValueError(message)
        for _generation, sectors in sorted(chain, key=lambda item: item[0]):
            for virtual_sector, physical in sectors:
                mapping[virtual_block * _SECTORS_PER_VIRTUAL_BLOCK + virtual_sector] = physical
    return mapping


def _logical_read(nand: PhysicalNand, mapping: dict[int, int], address: int, size: int) -> bytes:
    sector, column = divmod(address, _SECTOR_BYTES)
    if address < 0 or size < 0 or column + size > _SECTOR_BYTES:
        message = "unsupported RunningNV record extent"
        raise ValueError(message)
    if sector not in mapping:
        message = "current RunningNV sector is missing"
        raise ValueError(message)
    return nand.main_bytes(mapping[sector] + column, size)


def read_running_nv_record(
    nand: PhysicalNand,
    extent: tuple[int, int],
    *,
    identifier: int,
    size: int,
    virtual_blocks: int,
) -> bytes:
    """Require agreeing current indices and a checksum-valid short record."""
    _require_empty_remapping(nand)
    mapping = _sector_map(nand, extent, virtual_blocks)
    header = _logical_read(nand, mapping, 0, _VOLUME_HEADER.size)
    (
        magic,
        _header_word,
        minimum,
        maximum,
        sector_count,
        sector_bytes,
        record_count,
        index_bytes,
        header_copy,
        index_copy,
    ) = _VOLUME_HEADER.unpack(header)
    if (
        magic != 0x53544E56
        or sector_bytes != _SECTOR_BYTES
        or index_bytes != _INDEX.size
        or not minimum <= identifier <= maximum
        or record_count != maximum - minimum + 1
        or sector_count != virtual_blocks * _SECTORS_PER_VIRTUAL_BLOCK
        or header_copy != 1
        or index_copy != 1
    ):
        message = "unsupported RunningNV index format"
        raise ValueError(message)
    index_sectors = (record_count * index_bytes + sector_bytes - 1) // sector_bytes
    primary = 2
    backup = primary + index_sectors
    data_start = (backup + index_sectors) * sector_bytes
    data_end = sector_count * sector_bytes - sector_bytes * 128
    records: list[bytes] = []
    for index_start in (primary, backup):
        address = index_start * sector_bytes + (identifier - minimum) * index_bytes
        entry = _logical_read(nand, mapping, address, index_bytes)
        checksum, length, record_address, _flags, _auxiliary = _INDEX.unpack(entry)
        if checksum != nv_checksum(entry[2:]) or length != size:
            message = "invalid current RunningNV index entry"
            raise ValueError(message)
        if record_address < data_start or record_address + size > data_end:
            message = "current RunningNV data is outside its record region"
            raise ValueError(message)
        record = _logical_read(nand, mapping, record_address, _RECORD.size + size)
        header_crc, payload_crc, record_id, record_size, state = _RECORD.unpack_from(record)
        payload = record[_RECORD.size :]
        if (
            record_id != identifier
            or record_size != size
            or not state & 1
            or header_crc != nv_checksum(record[4 : _RECORD.size])
            or payload_crc != nv_checksum(payload)
        ):
            message = "invalid selected RunningNV record"
            raise ValueError(message)
        records.append(payload)
    if records[0] != records[1]:
        message = "current RunningNV indices disagree"
        raise ValueError(message)
    return records[0]

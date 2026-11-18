# SPDX-License-Identifier: GPL-2.0-only
"""Read selected records from indexed logical NV storage."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING, ClassVar

from .formats import nv_checksum

if TYPE_CHECKING:
    from collections.abc import Callable, Mapping

_VOLUME_HEADER = struct.Struct("<IIHHHHHHBB")
_INDEX = struct.Struct("<HHIII")
_RECORD = struct.Struct("<HHHHI")


@dataclass(frozen=True)
class NvVolume:
    """The common layout used by complete logical and mapped physical NV reads."""

    header_bytes: ClassVar[int] = _VOLUME_HEADER.size

    minimum: int
    maximum: int
    sector_count: int
    sector_bytes: int
    record_count: int

    @classmethod
    def from_header(cls, header: bytes) -> NvVolume:
        """Require the supported indexed layout without choosing a storage policy."""
        if len(header) < _VOLUME_HEADER.size:
            message = "unsupported RunningNV index format"
            raise ValueError(message)
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
        ) = _VOLUME_HEADER.unpack_from(header)
        if (
            magic != 0x53544E56
            or sector_bytes not in (512, 1024)
            or index_bytes != _INDEX.size
            or minimum > maximum
            or record_count != maximum - minimum + 1
            or header_copy != 1
            or index_copy != 1
        ):
            message = "unsupported RunningNV index format"
            raise ValueError(message)
        return cls(minimum, maximum, sector_count, sector_bytes, record_count)

    @property
    def index_sectors(self) -> int:
        """Return the sector-aligned size of each of the two record indices."""
        return (self.record_count * _INDEX.size + self.sector_bytes - 1) // self.sector_bytes

    @property
    def data_start(self) -> int:
        """Return the first byte after the duplicate headers and indices."""
        return (2 + 2 * self.index_sectors) * self.sector_bytes

    def read_record(
        self,
        read: Callable[[int, int], bytes],
        *,
        identifier: int,
        size: int,
        data_end: int,
    ) -> bytes:
        """Require both selected indices to yield the same checksum-valid payload."""
        if not self.minimum <= identifier <= self.maximum:
            message = "unsupported RunningNV index format"
            raise ValueError(message)
        records: list[bytes] = []
        for index_start in (2, 2 + self.index_sectors):
            address = index_start * self.sector_bytes + (identifier - self.minimum) * _INDEX.size
            entry = read(address, _INDEX.size)
            checksum, length, record_address, _flags, _auxiliary = _INDEX.unpack(entry)
            if checksum != nv_checksum(entry[2:]) or length != size:
                message = "invalid current RunningNV index entry"
                raise ValueError(message)
            if record_address < self.data_start or record_address + size > data_end:
                message = "current RunningNV data is outside its record region"
                raise ValueError(message)
            record = read(record_address, _RECORD.size + size)
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


def read_nv_volume_records(volume: bytes, required: Mapping[int, int]) -> dict[int, bytes]:
    """Select exact-size records from a complete volume with agreeing header/index copies."""
    layout = NvVolume.from_header(volume)
    if len(volume) != layout.sector_count * layout.sector_bytes or len(volume) < layout.data_start:
        message = "NV volume size does not match its declared sector count"
        raise ValueError(message)
    sector_bytes = layout.sector_bytes
    if volume[:sector_bytes] != volume[sector_bytes : 2 * sector_bytes]:
        message = "NV volume header copies disagree"
        raise ValueError(message)
    primary = 2 * sector_bytes
    backup = primary + layout.index_sectors * sector_bytes
    if volume[primary:backup] != volume[backup : layout.data_start]:
        message = "NV volume index copies disagree"
        raise ValueError(message)

    def read(address: int, size: int) -> bytes:
        if address < 0 or size < 0 or address + size > len(volume):
            message = "NV record extends beyond the complete volume"
            raise ValueError(message)
        return volume[address : address + size]

    return {
        identifier: layout.read_record(
            read, identifier=identifier, size=size, data_end=len(volume)
        )
        for identifier, size in required.items()
    }

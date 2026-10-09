# SPDX-License-Identifier: GPL-2.0-only
"""Read a CM4 load image and preserve the host's initial shared DDR policy."""

from __future__ import annotations

import struct
from dataclasses import dataclass

_LOAD_RECORD = struct.Struct("<4I")
_COMPRESSED_COPY = 0x3C
_RAW_COPY = 0x98
_ZERO_FILL = 0xB4
_LOAD_HANDLERS = (_COMPRESSED_COPY, _RAW_COPY, _ZERO_FILL)
_PUB_REGISTERS = 0xA12B0000


@dataclass(frozen=True)
class _LoadRegion:
    source: int
    destination: int
    size: int
    handler: int


def _halfwords(image: bytes, offset: int, count: int) -> tuple[int, ...]:
    if offset < 0 or offset + count * 2 > len(image):
        return ()
    return struct.unpack_from(f"<{count}H", image, offset)


def _compressed_end(image: bytes, source: int, limit: int, output_size: int) -> int:
    """Count a byte-oriented load stream without allocating its decoded contents."""
    cursor = source
    produced = 0

    def read_byte() -> int:
        nonlocal cursor
        if cursor >= limit:
            message = "truncated CM4 compressed load region"
            raise ValueError(message)
        value = image[cursor]
        cursor += 1
        return value

    while produced < output_size:
        token = read_byte()
        literals = token & 7
        if not literals:
            literals = read_byte()
        run = token >> 4
        if not run:
            run = read_byte()
        if not literals:
            message = "invalid CM4 compressed literal count"
            raise ValueError(message)
        literal_bytes = literals - 1
        cursor += literal_bytes
        if cursor > limit:
            message = "truncated CM4 compressed literals"
            raise ValueError(message)
        produced += literal_bytes
        if token & 8:
            distance = read_byte()
            if not 0 < distance <= produced:
                message = "invalid CM4 compressed back-reference"
                raise ValueError(message)
            run += 2
        if not literal_bytes + run or produced + run > output_size:
            message = "invalid CM4 compressed output length"
            raise ValueError(message)
        produced += run
    return cursor


def cm4_image(partition: bytes) -> bytes:
    """Return the load image through its table's initialized source extents.

    The startup code loads relative table bounds and dispatches copy and zero
    handlers. Zero-fill destination lengths do not describe bytes in the image.
    """
    # ADR/LDM and ADD instructions resolve the two relative table bounds.
    if _halfwords(partition, 8, 5) != (0xA00A, 0xE890, 0x0C00, 0x4482, 0x4483):
        message = "unsupported CM4 load-table instructions"
        raise ValueError(message)
    # Compressed copy, word copy and zero-fill handler entry instructions.
    if (
        _halfwords(partition, _COMPRESSED_COPY, 3) != (0x440A, 0xF04F, 0x0C00)
        or _halfwords(partition, _RAW_COPY, 4) != (0x3A10, 0xBF24, 0xC878, 0xC178)
        or _halfwords(partition, _ZERO_FILL, 4) != (0x2300, 0x2400, 0x2500, 0x2600)
    ):
        message = "unsupported CM4 load handlers"
        raise ValueError(message)
    begin_relative, end_relative = struct.unpack_from("<2I", partition, 0x34)
    begin, end = 0x34 + begin_relative, 0x34 + end_relative
    if begin % 4 or not begin < end <= len(partition) or (end - begin) % _LOAD_RECORD.size:
        message = "invalid CM4 load-table bounds"
        raise ValueError(message)
    regions = tuple(
        _LoadRegion(*values) for values in _LOAD_RECORD.iter_unpack(partition[begin:end])
    )
    bases = {regions[0].handler - handler for handler in _LOAD_HANDLERS}
    for region in regions:
        bases.intersection_update(region.handler - handler for handler in _LOAD_HANDLERS)
        if region.handler & 1 or not region.size or region.destination + region.size > 0x100000000:
            message = "invalid CM4 load-region destination"
            raise ValueError(message)
    if len(bases) != 1:
        message = "ambiguous or unsupported CM4 load address"
        raise ValueError(message)
    (base,) = bases
    initialized = tuple(region for region in regions if region.handler - base != _ZERO_FILL)
    if not initialized or base < 0 or base % 4:
        message = "CM4 load table has no initialized image regions"
        raise ValueError(message)
    sources = sorted({region.source - base for region in initialized})
    if sources[0] < 0 or sources[-1] >= len(partition):
        message = "CM4 load-region source is outside its partition"
        raise ValueError(message)
    source_limits = dict(zip(sources, [*sources[1:], len(partition)], strict=True))
    image_end = end
    for region in initialized:
        source = region.source - base
        if region.handler - base == _RAW_COPY:
            if region.size % 4:
                message = "unaligned CM4 raw load region"
                raise ValueError(message)
            region_end = source + region.size
            if region_end > len(partition):
                message = "truncated CM4 raw load region"
                raise ValueError(message)
        else:
            consumed_end = _compressed_end(partition, source, source_limits[source], region.size)
            region_end = (consumed_end + 3) & ~3
            if region_end > source_limits[source]:
                message = "CM4 compressed load regions overlap"
                raise ValueError(message)
        image_end = max(image_end, region_end)
    return partition[:image_end]


def _literal_word(image: bytes, offset: int, register: int) -> int | None:
    instruction = _halfwords(image, offset, 1)
    if not instruction or instruction[0] & 0xFF00 != 0x4800 | register << 8:
        return None
    literal = ((offset + 4) & ~3) + (instruction[0] & 0xFF) * 4
    if literal + 4 > len(image):
        return None
    return int(struct.unpack_from("<I", image, literal)[0])


def _call_target(image: bytes, offset: int) -> int | None:
    instructions = _halfwords(image, offset, 2)
    if len(instructions) != 2:
        return None
    first, second = instructions
    if first & 0xF800 != 0xF000 or second & 0xD000 != 0xD000:
        return None
    sign = first >> 10 & 1
    first_high = 1 ^ (second >> 13 & 1) ^ sign
    second_high = 1 ^ (second >> 11 & 1) ^ sign
    displacement = (
        sign << 24
        | first_high << 23
        | second_high << 22
        | (first & 0x3FF) << 12
        | (second & 0x7FF) << 1
    )
    if sign:
        displacement -= 1 << 25
    target = offset + 4 + displacement
    return target if 0 <= target < len(image) else None


def _bitfield_insert(image: bytes, offset: int) -> bool:
    # Build a width-bit mask, clear it at the selected shift, and merge the value.
    return _halfwords(image, offset, 14) == (
        0xF04F,
        0x0C01,
        0xFA0C,
        0xF202,
        0x1E52,
        0xFA02,
        0xFC01,
        0x401A,
        0xEA20,
        0x000C,
        0xFA02,
        0xF101,
        0x4308,
        0x4770,
    )


def _policy_offsets(image: bytes, start: int) -> tuple[int, int] | None:
    """Recognize the initialization frame and the two shared-register write spans."""
    if (
        _halfwords(image, start, 1) != (0xB538,)  # PUSH {r3, r4, r5, lr}
        or _literal_word(image, start + 2, 1) is None
        or _halfwords(image, start + 4, 2) != (0x2000, 0x9000)
        or _halfwords(image, start + 8, 6)
        != tuple(0x6008 | word_offset << 6 for word_offset in (2, 3, 7, 4, 5, 6))
    ):
        return None
    first = start + 20
    if (
        _literal_word(image, first, 4) != _PUB_REGISTERS
        or _halfwords(image, first + 2, 5) != (0xF8D4, 0x0230, 0x2301, 0x461A, 0x210A)
        or _halfwords(image, first + 16, 2) != (0xF8C4, 0x0230)
        or _literal_word(image, first + 20, 1) is None
        or _halfwords(image, first + 22, 1) != (0x2001,)
        or _call_target(image, first + 24) is None
    ):
        return None
    setter = _call_target(image, first + 12)
    if setter is None or not _bitfield_insert(image, setter):
        return None
    second = first + 28
    if (
        _halfwords(image, second, 1) != (0x6DE0,)  # LDR r0, [r4, #0x5c]
        or _literal_word(image, second + 2, 3) != 0x00010101
        or _halfwords(image, second + 4, 2) != (0x2218, 0x2100)
        or _call_target(image, second + 8) != setter
        or _halfwords(image, second + 12, 6) != (0x65E0, 0xF8D4, 0x00CC, 0x2300, 0x2201, 0x211C)
        or _call_target(image, second + 24) != setter
        or _halfwords(image, second + 28, 2) != (0xF8C4, 0x00CC)
        or _literal_word(image, second + 32, 0) != _PUB_REGISTERS + 0x1000
    ):
        return None
    # Remaining initial policy stores and the retained setup at the branch destination.
    if (
        _halfwords(image, second + 34, 24)
        != (
            0xF04F,
            0x7180,
            0x65C1,
            0x2101,
            0xF8C0,
            0x10C8,
            0xF8C0,
            0x1360,
            0xF04F,
            0x427F,
            0xF8C0,
            0x20E0,
            0xF8D4,
            0x2340,
            0xF012,
            0x0F01,
            0xBF08,
            0xF8C0,
            0x1340,
            0xF8D4,
            0x0130,
            0x2301,
            0x461A,
            0x2100,
        )
        or _call_target(image, second + 82) != setter
        or _halfwords(image, second + 86, 2) != (0xF8C4, 0x0130)
        or _literal_word(image, second + 90, 1) is None
        or _halfwords(image, second + 92, 1) != (0x2000,)
        or _call_target(image, second + 94) is None
    ):
        return None
    return first, second


def omit_initial_pub_policy(original: bytes) -> bytes:
    """Change only two verified branches in one compatible DDR initialization routine."""
    matches = []
    cursor = 0
    while (start := original.find(b"\x38\xb5", cursor)) != -1:
        cursor = start + 2
        if start % 2 == 0 and (offsets := _policy_offsets(original, start)) is not None:
            matches.append(offsets)
    if len(matches) != 1:
        raise ValueError(
            f"expected one supported CM4 DDR initialization routine, found {len(matches)}"
        )
    first, second = matches[0]
    prepared = bytearray(original)
    # Skip the first write span and the later policy span; retain both setup calls.
    struct.pack_into("<H", prepared, first, 0xE000 | (20 - 4) // 2)
    struct.pack_into("<H", prepared, second, 0xE000 | (90 - 4) // 2)
    return bytes(prepared)

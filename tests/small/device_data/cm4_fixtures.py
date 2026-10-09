# SPDX-License-Identifier: GPL-2.0-only
"""Small synthetic Thumb programs and load tables for CM4 reader tests."""

from __future__ import annotations

import struct


def load_image(*, table: int = 0xE0, base: int = 0x18000000) -> bytes:
    """Build raw, compressed and zero regions with distinct source and output sizes."""
    raw = table + 0x40
    image = bytearray(raw + 12)
    struct.pack_into("<5H", image, 8, 0xA00A, 0xE890, 0x0C00, 0x4482, 0x4483)
    struct.pack_into("<2I", image, 0x34, table - 0x34, table + 48 - 0x34)
    struct.pack_into("<3H", image, 0x3C, 0x440A, 0xF04F, 0x0C00)
    struct.pack_into("<4H", image, 0x98, 0x3A10, 0xBF24, 0xC878, 0xC178)
    struct.pack_into("<4H", image, 0xB4, 0x2300, 0x2400, 0x2500, 0x2600)
    entries = (
        (base + raw, 0x2000, 4, base + 0x98),
        (base + raw + 4, 0x2100, 8, base + 0x3C),
        (base + raw + 12, 0x2200, 0x10000, base + 0xB4),
    )
    for index, entry in enumerate(entries):
        struct.pack_into("<4I", image, table + index * 16, *entry)
    image[raw : raw + 4] = b"\xff" * 4
    # Two zeros followed by literals AB and four bytes copied at distance two.
    image[raw + 4 : raw + 9] = b"\x21\x2bAB\x02"
    return bytes(image)


def cm4_firmware(*, routine: int = 0x100) -> bytes:
    """Build a short initialization program with relocated literal pools and calls."""
    table = (routine + 0x1FF) & ~3
    image = bytearray(load_image(table=table))
    pool = (routine + 0xDF) & ~3
    setter = routine + 0x120
    callback = routine + 0x110

    def emit(offset: int, *halfwords: int) -> None:
        struct.pack_into(f"<{len(halfwords)}H", image, offset, *halfwords)

    def literal(offset: int, register: int, value: int) -> None:
        nonlocal pool
        distance = pool - ((offset + 4) & ~3)
        emit(offset, 0x4800 | register << 8 | distance // 4)
        struct.pack_into("<I", image, pool, value)
        pool += 4

    def call(offset: int, target: int) -> None:
        # All fixture calls are short, forward Thumb BL instructions.
        emit(offset, 0xF000, 0xF800 | (target - offset - 4) // 2)

    emit(routine, 0xB538)
    literal(routine + 2, 1, 0x20000000)
    emit(routine + 4, 0x2000, 0x9000, 0x6088, 0x60C8, 0x61C8, 0x6108, 0x6148, 0x6188)
    first = routine + 20
    literal(first, 4, 0xA12B0000)
    emit(first + 2, 0xF8D4, 0x0230, 0x2301, 0x461A, 0x210A)
    call(first + 12, setter)
    emit(first + 16, 0xF8C4, 0x0230)
    literal(first + 20, 1, 0x20000020)
    emit(first + 22, 0x2001)
    call(first + 24, callback)
    second = routine + 48
    emit(second, 0x6DE0)
    literal(second + 2, 3, 0x00010101)
    emit(second + 4, 0x2218, 0x2100)
    call(second + 8, setter)
    emit(second + 12, 0x65E0, 0xF8D4, 0x00CC, 0x2300, 0x2201, 0x211C)
    call(second + 24, setter)
    emit(second + 28, 0xF8C4, 0x00CC)
    literal(second + 32, 0, 0xA12B1000)
    emit(second + 34, 0xF04F, 0x7180, 0x65C1, 0x2101, 0xF8C0, 0x10C8)
    emit(second + 46, 0xF8C0, 0x1360, 0xF04F, 0x427F, 0xF8C0, 0x20E0)
    emit(second + 58, 0xF8D4, 0x2340, 0xF012, 0x0F01, 0xBF08, 0xF8C0, 0x1340)
    emit(second + 72, 0xF8D4, 0x0130, 0x2301, 0x461A, 0x2100)
    call(second + 82, setter)
    emit(second + 86, 0xF8C4, 0x0130)
    literal(second + 90, 1, 0x20000040)
    emit(second + 92, 0x2000)
    call(second + 94, callback)
    emit(callback, 0x4770)
    # Insert a value into a shifted field of the caller's word and return it.
    emit(setter, 0xF04F, 0x0C01, 0xFA0C, 0xF202, 0x1E52, 0xFA02, 0xFC01)
    emit(setter + 14, 0x401A, 0xEA20, 0x000C, 0xFA02, 0xF101, 0x4308, 0x4770)
    return bytes(image)

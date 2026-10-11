# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic integrated-speaker records with headphone-only Headfree playback."""

from __future__ import annotations

import struct

from tests.small.device_data.audio_fixtures import UNUSED_BAND, eq_set


def headphone_only_headfree_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Keep distinct route gains and PGA values in otherwise valid fixed records."""
    arm = bytearray(5360)
    modes = (
        (0, b"Headset", 0x12, 0x0400, 0x0800, 4, (95, 87, 79, 71, 63, 55, 47, 39, 31), 0),
        (3216, b"Handsfree", 0x22, 0x1000, 0x1000, 0, (87, 79, 71, 63, 55, 47, 39, 31, 23), 43),
        (1072, b"Headfree", 0x12, 0x0800, 0x1800, 6, (90, 82, 74, 66, 58, 50, 42, 34, 26), 26),
    )
    for offset, name, route, control, input_gain, analog, gains, pa in modes:
        arm[offset : offset + 16] = name.ljust(16, b"\0")
        struct.pack_into("<H", arm, offset + 20, route)
        struct.pack_into("<H", arm, offset + 36, 1)
        struct.pack_into("<H", arm, offset + 40, control)
        struct.pack_into("<Hh", arm, offset + 44, 15, input_gain)
        struct.pack_into("<H", arm, offset + 62, 9)
        for index, gain in enumerate(gains):
            struct.pack_into("<I", arm, offset + 68 + index * 4, gain << 16 | analog)
        struct.pack_into("<H", arm, offset + 466, pa)
    eq = bytearray(8160)
    for position, name, ratio in (
        (1, b"EQ_Headset", 26398),
        (4, b"EQ_Handsfree", 26214),
        (2, b"EQ_Headfree", 26000),
    ):
        eq[(position - 1) * 544 : position * 544] = eq_set(
            name, band_control=0, bands=(UNUSED_BAND,) * 7, alc_ratio=ratio
        )
    records = {425: b"\x05\x02", 426: bytes(arm), 440: bytes(eq)}
    return records, records.copy()

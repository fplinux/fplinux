# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic audio input fixtures."""

from __future__ import annotations

import struct

INOI_HANDSFREE_VIBRATE_TONE_OFFSET = 3 * 1072 + 188


def eq_set(
    name: bytes,
    *,
    band_control: int,
    bands: tuple[tuple[int, int, int, int], ...],
    alc_ratio: int = 26398,
) -> bytes:
    """Serialize one NV440 EQ set from its literal pass-mode fields.

    ``bands`` are the raw pass-mode records {centre Hz, Q * 512, boost, base
    gain in 0.1 dB}; records 5 and 6 hold the low-cut shelves. As in the fitted
    music sets, the control word enables ALC with the full-scale output limit
    and five bands, and the pass-mode input gain is unity.
    """
    eq_set = bytearray(544)
    eq_set[:16] = name.ljust(16, b"\0")
    struct.pack_into("<HhH", eq_set, 16, 0x017F, 0x1000, band_control)
    for index, band in enumerate(bands):
        struct.pack_into("<4h", eq_set, 22 + 8 * index, *band)
    # Hold, rise, fall, limit, threshold, ratio, gain variation, release, attack,
    # extended release and extended attack.
    struct.pack_into("<11h", eq_set, 522, 480, 8192, 1278, 0, -30, alc_ratio, 652, 16, 1245, 0, 98)
    return bytes(eq_set)


UNUSED_BAND = (0, 0, 0, 0)

# The fitted EQ sets at the NV440 positions that the playback modes select.
NOKIA_EQ_SETS = {
    1: eq_set(
        b"EQ_Headset",
        band_control=0xF800,
        bands=(
            (100, 512, -30, 0),
            (600, 256, 10, 0),
            (1000, 1536, 10, 0),
            (3000, 1024, 10, 0),
            (15500, 410, -40, 0),
            (-360, 0, 30, 0),
            (0, 3000, 0, 0),
        ),
    ),
    2: eq_set(
        b"EQ_Headfree",
        band_control=0xF880,
        bands=(
            (175, 1536, -120, 0),
            (100, 1536, -150, 0),
            (500, 256, 20, 0),
            (3000, 256, -10, 0),
            (15000, 359, -35, 0),
            (-180, 0, 340, 0),
            (0, 3000, 300, 0),
        ),
    ),
    4: eq_set(
        b"EQ_Handsfree",
        band_control=0xF880,
        bands=(
            (550, 1024, -20, 0),
            (850, 1024, 0, 0),
            (2600, 1024, 0, 0),
            (3500, 1024, 0, 0),
            (22000, 2048, -200, 0),
            (-120, 0, 200, 0),
            (0, 3000, 300, 0),
        ),
    ),
}

INOI_EQ_SETS = {
    1: eq_set(
        b"EQ_Headset",
        band_control=0x0000,
        bands=(*(UNUSED_BAND,) * 5, (-360, 0, 30, 0), (0, 3000, 0, 0)),
    ),
    2: eq_set(
        b"EQ_Headfree",
        band_control=0x8080,
        bands=(
            (150, 2560, -120, 0),
            *(UNUSED_BAND,) * 4,
            (-180, 0, 400, 0),
            (0, 2000, 300, 0),
        ),
        alc_ratio=26214,
    ),
    4: eq_set(
        b"EQ_Handsfree",
        band_control=0xF080,
        bands=(
            (170, 2560, -540, 0),
            (22000, 2560, 50, 0),
            (2000, 512, 30, 0),
            (3000, 512, 0, 0),
            UNUSED_BAND,
            (-180, 0, 400, 0),
            (0, 2000, 300, 0),
        ),
    ),
}


def store_music_processing(arm: bytearray, offset: int, *, control: int, input_gain: int) -> None:
    """Store a mode's processing-control word, player-selected EQ switch and input gain.

    Bits 14:10 of the fitted control words select the music EQ set; the lower
    fields select the MIDI and AMR sets.
    """
    struct.pack_into("<H", arm, offset + 40, control)
    struct.pack_into("<Hh", arm, offset + 44, 0x000F, input_gain)


def headset_audio_records(
    levels: tuple[int, ...],
    *,
    input_gain: int,
    eq_sets: dict[int, bytes],
) -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Build matching fitted Headset records from explicit packed NV level values.

    ``eq_sets`` maps one-based NV440 positions to serialized sets. The copies
    differ only in a mode and an EQ set that playback does not use.
    """
    if len(levels) != 9:
        message = "the synthetic Headset fixture requires nine levels"
        raise ValueError(message)
    arm = bytearray(5360)
    arm[:16] = b"Headset".ljust(16, b"\0")
    struct.pack_into("<H", arm, 36, 1)
    store_music_processing(arm, 0, control=0x04CB, input_gain=input_gain)
    struct.pack_into("<H", arm, 62, 9)
    struct.pack_into("<9I", arm, 68, *levels)
    eq = bytearray(8160)
    for position, eq_set in eq_sets.items():
        eq[(position - 1) * 544 : position * 544] = eq_set
    downloaded = {425: b"\x05\x02", 426: bytes(arm), 440: bytes(eq)}
    protected_arm = bytearray(arm)
    protected_arm[2 * 1072] = 1
    protected_eq = bytearray(eq)
    protected_eq[11 * 544] = 1
    protected = {425: b"\x05\x02", 426: bytes(protected_arm), 440: bytes(protected_eq)}
    return downloaded, protected


def add_fitted_headfree_mode(records: dict[int, bytes], *, input_gain: int) -> None:
    """Store the literal Headfree mode that all three fitted phones keep at index 1."""
    arm = bytearray(records[426])
    offset = 1072
    arm[offset : offset + 16] = b"Headfree".ljust(16, b"\0")
    struct.pack_into("<H", arm, offset + 20, 0x0032)
    struct.pack_into("<H", arm, offset + 36, 1)
    store_music_processing(arm, offset, control=0x08EC, input_gain=input_gain)
    struct.pack_into("<H", arm, offset + 62, 9)
    # The fitted level-0 word precedes volume levels 1..9 and is not a volume step.
    struct.pack_into(
        "<10I",
        arm,
        offset + 64,
        0x0018009F,
        0x004E0070,
        0x00470070,
        0x003B0070,
        0x00350070,
        0x002F0070,
        0x002A0070,
        0x00240070,
        0x001F0070,
        0x001A0070,
    )
    struct.pack_into("<H", arm, offset + 466, 0x001A)
    records[426] = bytes(arm)


def inoi_audio_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Use the literal playback-mode, EQ-set and vibrate-tone values of both fitted INOI phones."""
    downloaded, protected = headset_audio_records(
        (
            0x006C0007,
            0x00610007,
            0x00560007,
            0x004B0007,
            0x00410007,
            0x00370007,
            0x002D0007,
            0x00230007,
            0x001B0007,
        ),
        input_gain=0x071D,
        eq_sets=INOI_EQ_SETS,
    )
    handsfree_levels = (
        0x00390000,
        0x00350000,
        0x00310000,
        0x002D0000,
        0x00290000,
        0x00210000,
        0x001D0000,
        0x00190000,
        0x00170000,
    )
    for records in (downloaded, protected):
        arm = bytearray(records[426])
        offset = 3 * 1072
        arm[offset : offset + 16] = b"Handsfree".ljust(16, b"\0")
        struct.pack_into("<H", arm, offset + 20, 0x0022)
        struct.pack_into("<H", arm, offset + 36, 1)
        store_music_processing(arm, offset, control=0x112E, input_gain=0x0CA6)
        struct.pack_into("<H", arm, offset + 62, 9)
        struct.pack_into("<9I", arm, offset + 68, *handsfree_levels)
        struct.pack_into(
            "<9H",
            arm,
            INOI_HANDSFREE_VIBRATE_TONE_OFFSET,
            *(0xFDCD, 0x232B, 0x3FF6, 0x5428, 0x0082, 0x0082, 0x0002, 0x0008, 0x0205),
        )
        struct.pack_into("<H", arm, offset + 466, 0x001A)
        records[426] = bytes(arm)
        add_fitted_headfree_mode(records, input_gain=0x0400)
    return downloaded, protected


def nokia_audio_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Use the fitted Nokia Headset, Headfree and speaker-only Handsfree values and EQ sets."""
    downloaded, protected = headset_audio_records(
        (
            0x00470006,
            0x00420006,
            0x003D0006,
            0x00370006,
            0x00320006,
            0x002C0006,
            0x00260006,
            0x00200006,
            0x001A0006,
        ),
        input_gain=0x05A6,
        eq_sets=NOKIA_EQ_SETS,
    )
    handsfree_levels = (
        0x00380000,
        0x00340000,
        0x00300000,
        0x002C0000,
        0x00280000,
        0x00240000,
        0x00200000,
        0x001C0000,
        0x001B0000,
    )
    for records in (downloaded, protected):
        arm = bytearray(records[426])
        arm[3 * 1072 : 3 * 1072 + 16] = b"Handsfree".ljust(16, b"\0")
        struct.pack_into("<H", arm, 3 * 1072 + 20, 0x0022)
        struct.pack_into("<H", arm, 3 * 1072 + 36, 1)
        # Its AMR field selects EQ set 1, and its MIDI and AMR input gains differ.
        store_music_processing(arm, 3 * 1072, control=0x1121, input_gain=0x0A0C)
        struct.pack_into("<2h", arm, 3 * 1072 + 48, 0x2800, 0x08DA)
        struct.pack_into("<H", arm, 3 * 1072 + 62, 9)
        struct.pack_into("<9I", arm, 3 * 1072 + 68, *handsfree_levels)
        struct.pack_into("<H", arm, 3 * 1072 + 466, 0x001A)
        records[426] = bytes(arm)
        add_fitted_headfree_mode(records, input_gain=0x071D)
    return downloaded, protected

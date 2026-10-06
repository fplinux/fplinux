# SPDX-License-Identifier: GPL-2.0-only
"""Independent expected audio bytes and register words."""

from __future__ import annotations

import struct

# The fitted INOI 175 Hz tone: -sin words for 24, 32 and 48 kHz, cos words for
# the same rates, then gain 0, gain 1, gain down, gain up and hold.
INOI_VIBRATE_TONE_SECTION = bytes.fromhex(
    "a1a111fd 2b23cdfd 9cb788fe d7ceee3f 2854f63f 91b3fb3f 8200 8200 0200 0800 0502"
)

# The Headfree section shared by all three fitted phones: PA word 0x001a,
# headphone PGA 7, then the digital gains of volume levels 1..9.
FITTED_HEADFREE_SECTION = bytes.fromhex("1a00 07 4e 47 3b 35 2f 2a 24 1f 1a")

# The processing section follows the 42-byte header and the 11-byte speaker and
# 12-byte combined sections; each playback route in it has the same size.
PROCESSING_OFFSET = 65

PROCESSING_ROUTE_SIZE = 241

# An EQ6 section as the signed register words S, B0, B1, -A1, B2, -A2; this one
# passes audio unchanged.
PASS_THROUGH = (4096, 16384, 0, 0, 0, 0)

# Expected register words of the fitted pass-mode sections at 24, 32 and 48 kHz,
# computed independently of the production generator. Section 0 is the low-cut
# filter; sections 1..5 are bands 0..4.
NOKIA_HEADSET_SECTIONS = (
    # 24 kHz: the 15.5 kHz band is not below half the rate and passes through.
    (
        PASS_THROUGH,
        (4096, 16309, -32259, 32259, 15961, -15887),
        (4096, 16640, -28170, 28170, 11880, -12136),
        (4096, 16459, -30424, 30424, 15037, -15113),
        (4096, 16696, -19509, 19509, 10894, -11206),
        PASS_THROUGH,
    ),
    # 32 kHz
    (
        PASS_THROUGH,
        (4096, 16330, -32411, 32411, 16088, -16035),
        (4096, 16579, -29287, 29287, 12911, -13107),
        (4096, 16441, -31195, 31195, 15365, -15423),
        (4096, 16625, -23899, 23899, 12119, -12361),
        (4096, 10893, 3025, -3025, -7855, 13344),
    ),
    # 48 kHz
    (
        PASS_THROUGH,
        (4096, 16345, -32528, 32528, 16185, -16147),
        (4096, 16520, -30421, 30421, 13996, -14132),
        (4096, 16421, -31849, 31849, 15701, -15739),
        (4096, 16549, -27698, 27698, 13433, -13598),
        (4096, 11541, 2893, -2893, -4997, 9838),
    ),
)

NOKIA_HANDSFREE_SECTIONS = (
    # 24 kHz: the 0 dB bands and the 22 kHz band pass through.
    (
        (4096, 16074, -22532, 22349, 6577, -6447),
        (4096, 16253, -31199, 31199, 15271, -15140),
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
    ),
    # 32 kHz
    (
        (4096, 16154, -24642, 24534, 8557, -8433),
        (4096, 16282, -31633, 31633, 15534, -15433),
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
    ),
    # 48 kHz
    (
        (4096, 16228, -26972, 26919, 10779, -10673),
        (4096, 16316, -32051, 32051, 15818, -15750),
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
        (4096, 8373, 14455, -14455, 6595, 1415),
    ),
)

NOKIA_HEADFREE_SECTIONS = (
    # 24 kHz: the 15 kHz band is not below half the rate and passes through.
    (
        (4096, 15772, -22139, 21780, 6466, -6212),
        (4096, 16217, -32293, 32293, 16109, -15942),
        (4096, 16265, -32473, 32473, 16219, -16101),
        (4096, 16822, -29088, 29088, 12516, -12955),
        (4096, 15460, -11250, 11250, 450, 473),
        PASS_THROUGH,
    ),
    # 32 kHz
    (
        (4096, 15926, -24316, 24102, 8449, -8202),
        (4096, 16258, -32416, 32416, 16178, -16052),
        (4096, 16288, -32533, 32533, 16252, -16157),
        (4096, 16722, -29980, 29980, 13402, -13741),
        (4096, 15642, -15953, 15953, 3546, -2804),
        (4096, 11455, 3026, -3026, -8371, 13298),
    ),
    # 48 kHz
    (
        (4096, 16071, -26727, 26623, 10685, -10476),
        (4096, 16310, -32567, 32567, 16266, -16193),
        (4096, 16335, -32650, 32650, 16319, -16270),
        (4096, 16614, -30902, 30902, 14354, -14584),
        (4096, 15835, -21041, 21041, 6941, -6392),
        (4096, 11615, 1550, -1550, -7563, 12330),
    ),
)

INOI_HEADSET_SECTIONS = ((PASS_THROUGH,) * 6,) * 3

INOI_HANDSFREE_SECTIONS = (
    # 24 kHz: the 22 kHz band and the 0 dB band pass through.
    (
        (4096, 15676, -24535, 24235, 8943, -8533),
        (4096, 15138, -30241, 30241, 15134, -13888),
        PASS_THROUGH,
        (4096, 17622, -23169, 23169, 9124, -10362),
        PASS_THROUGH,
        PASS_THROUGH,
    ),
    # 32 kHz
    (
        (4096, 15848, -26286, 26107, 10490, -10130),
        (4096, 15371, -30724, 30724, 15369, -14357),
        PASS_THROUGH,
        (4096, 17349, -25935, 25935, 10723, -11689),
        PASS_THROUGH,
        PASS_THROUGH,
    ),
    # 48 kHz
    (
        (4096, 16024, -28232, 28148, 12232, -11953),
        (4096, 15861, -31712, 31712, 15861, -15338),
        (4096, 18695, 25902, -25902, 8125, -10437),
        (4096, 17052, -28507, 28507, 12460, -13128),
        PASS_THROUGH,
        PASS_THROUGH,
    ),
)

INOI_HEADFREE_SECTIONS = (
    # 24 kHz: only the low-cut filter and the 150 Hz band are enabled.
    (
        (4096, 15676, -24535, 24235, 8943, -8533),
        (4096, 16293, -32504, 32504, 16237, -16146),
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
    ),
    # 32 kHz
    (
        (4096, 15848, -26286, 26107, 10490, -10130),
        (4096, 16328, -32610, 32610, 16296, -16240),
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
    ),
    # 48 kHz
    (
        (4096, 16024, -28232, 28148, 12232, -11953),
        (4096, 16346, -32664, 32664, 16326, -16288),
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
        PASS_THROUGH,
    ),
)


def processing_route(
    output_scale: int,
    alc_ratio: int,
    sections_by_rate: tuple[tuple[tuple[int, ...], ...], ...],
) -> bytes:
    """Pack one fitted playback route of the processing section.

    A route holds S6, the ALC enable and its eleven words, then six sections
    for each of 24, 32 and 48 kHz. Every fitted route enables ALC with the same
    words apart from the ratio.
    """
    route = struct.pack(
        "<HB11h", output_scale, 1, 480, 8192, 1278, 0, -30, alc_ratio, 652, 16, 1245, 0, 98
    )
    for sections in sections_by_rate:
        for section in sections:
            route += struct.pack("<6h", *section)
    return route


# Headset, Handsfree and Headfree routes with their fitted S6 output scales.
FITTED_NOKIA_PROCESSING_SECTION = (
    processing_route(0x05A6, 26398, NOKIA_HEADSET_SECTIONS)
    + processing_route(0x0A0C, 26398, NOKIA_HANDSFREE_SECTIONS)
    + processing_route(0x071D, 26398, NOKIA_HEADFREE_SECTIONS)
)

FITTED_INOI_PROCESSING_SECTION = (
    processing_route(0x071D, 26398, INOI_HEADSET_SECTIONS)
    + processing_route(0x0CA6, 26398, INOI_HANDSFREE_SECTIONS)
    + processing_route(0x0400, 26214, INOI_HEADFREE_SECTIONS)
)

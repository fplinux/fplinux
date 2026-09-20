# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the compact headphone gain profile from fitted fixed-NV records."""

from __future__ import annotations

import struct
from itertools import pairwise
from typing import TYPE_CHECKING

from fplinux_cli.device_data import PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping

AUDIO_RECORD_SIZES = {425: 2, 426: 5360, 440: 8160}
_ARM_MODE_SIZE = 1072
_HEADSET_EQ_SIZE = 544
_SOURCE_PASS_EQ_BYPASS = 0
_SOURCE_PASS_EQ_ACTIVE_OMITTED = 1


def prepare_headset_gain_profile(
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    prefix: str,
    machine_compatible: bytes,
) -> PreparedGroup:
    """Normalize fitted Headset app-0 gains into the compact kernel input."""
    mode_count_record = downloaded[425]
    protected_mode_count = protected[425]
    arm_modes = downloaded[426]
    protected_arm_modes = protected[426]
    eq_sets = downloaded[440]
    protected_eq_sets = protected[440]

    mode_count = mode_count_record[0]
    if mode_count_record != protected_mode_count:
        message = "audio-profile mode count differs between DownloadedNV and ProtectNV"
        raise ValueError(message)
    if mode_count != len(arm_modes) // _ARM_MODE_SIZE:
        message = "audio-profile mode count does not match NV426"
        raise ValueError(message)
    if arm_modes[:16] != b"Headset".ljust(16, b"\0"):
        message = "audio-profile NV426 does not begin with the Headset mode"
        raise ValueError(message)
    if arm_modes[:_ARM_MODE_SIZE] != protected_arm_modes[:_ARM_MODE_SIZE]:
        message = "audio-profile Headset NV426 differs between DownloadedNV and ProtectNV"
        raise ValueError(message)
    if eq_sets[:16] != b"EQ_Headset".ljust(16, b"\0"):
        message = "audio-profile NV440 does not begin with the Headset EQ set"
        raise ValueError(message)
    if eq_sets[:_HEADSET_EQ_SIZE] != protected_eq_sets[:_HEADSET_EQ_SIZE]:
        message = "audio-profile Headset NV440 differs between DownloadedNV and ProtectNV"
        raise ValueError(message)

    valid_app_count = struct.unpack_from("<H", arm_modes, 36)[0]
    if valid_app_count < 1:
        message = "audio-profile Headset has no app 0"
        raise ValueError(message)
    app0 = 44
    level_count = struct.unpack_from("<H", arm_modes, app0 + 18)[0]
    if level_count != 9:
        raise ValueError(f"audio-profile Headset app 0 has {level_count} levels; expected 9")
    levels = struct.unpack_from("<9I", arm_modes, app0 + 24)
    headphone_pga = [value & 0xFFFF for value in levels]
    if len(set(headphone_pga)) != 1:
        message = "audio-profile Headset PGA levels 1..9 differ"
        raise ValueError(message)
    if not 2 <= headphone_pga[0] <= 7:
        message = "audio-profile Headset PGA level is outside supported range 2..7"
        raise ValueError(message)
    digital_gain = [value >> 16 for value in levels]
    if any(level > 127 for level in digital_gain):
        message = "audio-profile Headset digital gain exceeds 127"
        raise ValueError(message)
    if any(first < second for first, second in pairwise(digital_gain)):
        message = "audio-profile Headset digital gain is not monotonically decreasing"
        raise ValueError(message)

    pass_band_control = struct.unpack_from("<H", eq_sets, 20)[0]
    source_eq = _SOURCE_PASS_EQ_ACTIVE_OMITTED if pass_band_control else _SOURCE_PASS_EQ_BYPASS

    profile = struct.pack(
        "<8s24sBB9B",
        b"FPAUDIO\0",
        machine_compatible,
        headphone_pga[0],
        source_eq,
        *digital_gain,
    )
    originals = {
        f"{prefix}-nv425.bin": mode_count_record,
        f"{prefix}-nv426.bin": arm_modes,
        f"{prefix}-nv440.bin": eq_sets,
    }
    return PreparedGroup(
        originals=originals,
        prepared={f"{prefix}-audio-profile.bin": profile},
    )

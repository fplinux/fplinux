# SPDX-License-Identifier: GPL-2.0-only
"""Prepare compact playback gains from fitted fixed-NV records."""

from __future__ import annotations

import math
import struct
from itertools import pairwise
from typing import TYPE_CHECKING

from fplinux_cli.device_data import PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping

AUDIO_RECORD_SIZES = {425: 2, 426: 5360, 440: 8160}
_ARM_MODE_SIZE = 1072
_HEADSET_EQ_SIZE = 544
_MODE_NAME_SIZE = 16
_MODE_PLAY_DEV_SET_OFFSET = 20
_MODE_APP_COUNT_OFFSET = 36
_MODE_LEVEL_COUNT_OFFSET = 62
_MODE_LEVELS_OFFSET = 68
_MODE_VIBRATE_TONE_OFFSET = 188
_MODE_PA_WORD_OFFSET = 466
_PLAY_ROUTE_MASK = 0x30
_PLAY_SPEAKER_ONLY = 0x20
_PLAY_HEADPHONE_AND_SPEAKER = 0x30
_SOURCE_PASS_EQ_BYPASS = 0
_SOURCE_PASS_EQ_ACTIVE_OMITTED = 1
_VIBRATE_TONE_UNIT = 1 << 30
_VIBRATE_TONE_NORM_TOLERANCE = 1e-6
_VIBRATE_TONE_NV_SAMPLE_RATE = 32000
_VIBRATE_TONE_SAMPLE_RATES = (24000, 32000, 48000)


def vibrate_tone_words(frequency: float, sample_rate: int) -> tuple[int, int]:
    """Return the VBC vibrate-tone sine and cosine register words.

    The generator rotates a Q30 unit vector by one step per DAC sample. Stock
    firmware stores -sin and cos of that step rounded to nearest as 32-bit
    two's-complement words; truncation does not reproduce its tables.
    """
    step = 2 * math.pi * frequency / sample_rate
    sine = round(-math.sin(step) * _VIBRATE_TONE_UNIT)
    cosine = round(math.cos(step) * _VIBRATE_TONE_UNIT)
    return sine & 0xFFFFFFFF, cosine & 0xFFFFFFFF


def _signed32(word: int) -> int:
    return word - (1 << 32) if word & 0x80000000 else word


def _admitted_mode(arm_modes: bytes, protected_arm_modes: bytes, name: str) -> bytes:
    """Return the single named mode when both fixed-NV copies agree on it."""
    mode_name = name.encode().ljust(_MODE_NAME_SIZE, b"\0")
    matching_offsets = [
        offset
        for offset in range(0, len(arm_modes), _ARM_MODE_SIZE)
        if arm_modes[offset : offset + _MODE_NAME_SIZE] == mode_name
    ]
    if len(matching_offsets) != 1:
        raise ValueError(f"audio-profile NV426 requires exactly one {name} mode")
    offset = matching_offsets[0]
    mode = arm_modes[offset : offset + _ARM_MODE_SIZE]
    if mode != protected_arm_modes[offset : offset + _ARM_MODE_SIZE]:
        raise ValueError(f"audio-profile {name} NV426 differs between DownloadedNV and ProtectNV")
    return mode


def _app0_volume_levels(mode: bytes, name: str) -> tuple[list[int], list[int]]:
    """Return app-0 digital gains and analog words for volume levels 1..9.

    Each level word carries the digital gain in its high half and the mode's
    analog gain levels in its low half.
    """
    if struct.unpack_from("<H", mode, _MODE_APP_COUNT_OFFSET)[0] < 1:
        raise ValueError(f"audio-profile {name} has no app 0")
    level_count = struct.unpack_from("<H", mode, _MODE_LEVEL_COUNT_OFFSET)[0]
    if level_count != 9:
        raise ValueError(f"audio-profile {name} app 0 has {level_count} levels; expected 9")
    levels = struct.unpack_from("<9I", mode, _MODE_LEVELS_OFFSET)
    digital_gain = [level >> 16 for level in levels]
    if any(gain > 127 for gain in digital_gain):
        raise ValueError(f"audio-profile {name} digital gain exceeds 127")
    if any(first < second for first, second in pairwise(digital_gain)):
        raise ValueError(f"audio-profile {name} digital gain is not monotonically decreasing")
    analog_levels = [level & 0xFFFF for level in levels]
    return digital_gain, analog_levels


def _handsfree_speaker_fields(mode: bytes) -> bytes:
    """Admit speaker-only Handsfree playback and pack its PA and app-0 gains."""
    play_dev_set = struct.unpack_from("<H", mode, _MODE_PLAY_DEV_SET_OFFSET)[0]
    if play_dev_set & _PLAY_ROUTE_MASK != _PLAY_SPEAKER_ONLY:
        message = "audio-profile Handsfree does not select speaker-only playback"
        raise ValueError(message)
    digital_gain, analog_levels = _app0_volume_levels(mode, "Handsfree")
    if any(analog_levels):
        message = "audio-profile Handsfree PA gain is not the supported zero setting"
        raise ValueError(message)

    pa_word = struct.unpack_from("<H", mode, _MODE_PA_WORD_OFFSET)[0]
    return struct.pack("<H9B", pa_word, *digital_gain)


def _headfree_combined_fields(mode: bytes) -> bytes:
    """Admit headphone-and-speaker Headfree playback and pack its PA, PGA and gains.

    The analog word of every level holds the headphone PGA level in bits 7:4
    and the speaker PA gain level in bits 3:0. Only a fixed PGA level and the
    zero PA gain setting are admitted, matching the Headset and Handsfree data.
    """
    play_dev_set = struct.unpack_from("<H", mode, _MODE_PLAY_DEV_SET_OFFSET)[0]
    if play_dev_set & _PLAY_ROUTE_MASK != _PLAY_HEADPHONE_AND_SPEAKER:
        message = "audio-profile Headfree does not select headphone-and-speaker playback"
        raise ValueError(message)
    digital_gain, analog_levels = _app0_volume_levels(mode, "Headfree")
    if len(set(analog_levels)) != 1:
        message = "audio-profile Headfree analog levels 1..9 differ"
        raise ValueError(message)
    analog_level = analog_levels[0]
    if analog_level >> 8:
        message = "audio-profile Headfree analog level sets bits above the headphone PGA"
        raise ValueError(message)
    if analog_level & 0x0F:
        message = "audio-profile Headfree PA gain is not the supported zero setting"
        raise ValueError(message)
    headphone_pga = analog_level >> 4
    if not 2 <= headphone_pga <= 7:
        message = "audio-profile Headfree headphone PGA level is outside supported range 2..7"
        raise ValueError(message)

    pa_word = struct.unpack_from("<H", mode, _MODE_PA_WORD_OFFSET)[0]
    return struct.pack("<HB9B", pa_word, headphone_pga, *digital_gain)


def _handsfree_vibrate_tone_fields(mode: bytes) -> bytes:
    """Convert the fitted 32 kHz Handsfree vibrate tone for each DAC sample rate."""
    (sin_hi, sin_lo, cos_hi, cos_lo, gain_0, gain_1, gain_down, gain_up, hold) = (
        struct.unpack_from("<9H", mode, _MODE_VIBRATE_TONE_OFFSET)
    )
    if gain_0 == 0 or gain_1 == 0:
        message = "audio-profile Handsfree vibrate tone gain is zero"
        raise ValueError(message)
    if hold == 0:
        message = "audio-profile Handsfree vibrate tone hold is zero"
        raise ValueError(message)

    fitted_words = (sin_hi << 16 | sin_lo, cos_hi << 16 | cos_lo)
    sine = _signed32(fitted_words[0])
    cosine = _signed32(fitted_words[1])
    norm = math.hypot(sine, cosine)
    if not math.isclose(norm, _VIBRATE_TONE_UNIT, rel_tol=_VIBRATE_TONE_NORM_TOLERANCE):
        message = "audio-profile Handsfree vibrate tone sine and cosine are not a unit rotation"
        raise ValueError(message)
    frequency = math.atan2(-sine, cosine) * _VIBRATE_TONE_NV_SAMPLE_RATE / (2 * math.pi)
    if not 0 < frequency < _VIBRATE_TONE_NV_SAMPLE_RATE / 2:
        raise ValueError(
            f"audio-profile Handsfree vibrate tone {frequency:.2f} Hz is not between 0 and "
            f"{_VIBRATE_TONE_NV_SAMPLE_RATE // 2} Hz"
        )
    if vibrate_tone_words(frequency, _VIBRATE_TONE_NV_SAMPLE_RATE) != fitted_words:
        message = "audio-profile Handsfree vibrate tone is not reproducible at 32000 Hz"
        raise ValueError(message)

    rate_words = [vibrate_tone_words(frequency, rate) for rate in _VIBRATE_TONE_SAMPLE_RATES]
    sines = [sine_word for sine_word, _cosine_word in rate_words]
    cosines = [cosine_word for _sine_word, cosine_word in rate_words]
    return struct.pack("<3I3I5H", *sines, *cosines, gain_0, gain_1, gain_down, gain_up, hold)


def prepare_headset_gain_profile(
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    prefix: str,
    machine_compatible: bytes,
    speaker_vibration: bool = False,
) -> PreparedGroup:
    """Normalize fitted playback gains into the compact kernel input.

    ``speaker_vibration`` appends the Handsfree vibrate tone for phones whose
    speaker is also the vibration actuator.
    """
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
    handsfree = _admitted_mode(arm_modes, protected_arm_modes, "Handsfree")
    headfree = _admitted_mode(arm_modes, protected_arm_modes, "Headfree")
    profile += _handsfree_speaker_fields(handsfree)
    profile += _headfree_combined_fields(headfree)
    if speaker_vibration:
        profile += _handsfree_vibrate_tone_fields(handsfree)
    originals = {
        f"{prefix}-nv425.bin": mode_count_record,
        f"{prefix}-nv426.bin": arm_modes,
        f"{prefix}-nv440.bin": eq_sets,
    }
    return PreparedGroup(
        originals=originals,
        prepared={f"{prefix}-audio-profile.bin": profile},
    )

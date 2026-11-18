# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the compact playback profile from fitted NV records.

The profile carries the fitted playback gains and, for each playback route,
the stock DAC processing: the VBC EQ6 equalizer sections for every DAC sample
rate, the output scale S6 and the ALC settings.
"""

from __future__ import annotations

import math
import struct
from typing import TYPE_CHECKING

from fplinux_cli.device_data.audio_eq import admit_playback_processing, playback_processing_fields
from fplinux_cli.device_data.formats import PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

AUDIO_RECORD_SIZES = {425: 2, 426: 5360, 440: 8160}
_ARM_MODE_SIZE = 1072
_MODE_NAME_SIZE = 16
_MODE_PLAY_DEV_SET_OFFSET = 20
_MODE_APP_COUNT_OFFSET = 36
_MODE_LEVEL_COUNT_OFFSET = 62
_MODE_LEVELS_OFFSET = 68
_MODE_VIBRATE_TONE_OFFSET = 188
_MODE_PA_WORD_OFFSET = 466
_PLAY_ROUTE_MASK = 0x30
_PLAY_HEADPHONES_ONLY = 0x10
_PLAY_SPEAKER_ONLY = 0x20
_PLAY_HEADPHONE_AND_SPEAKER = 0x30
_VIBRATE_TONE_UNIT = 1 << 30
_VIBRATE_TONE_NORM_TOLERANCE = 1e-6
_VIBRATE_TONE_NV_SAMPLE_RATE = 32000
# The kernel selects per-rate data in this order.
_DAC_SAMPLE_RATES = (24000, 32000, 48000)


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


def _playback_mode_offset(arm_modes: bytes, name: str) -> int:
    """Return the offset of the single named playback mode."""
    mode_name = name.encode().ljust(_MODE_NAME_SIZE, b"\0")
    matching_offsets = [
        offset
        for offset in range(0, len(arm_modes), _ARM_MODE_SIZE)
        if arm_modes[offset : offset + _MODE_NAME_SIZE] == mode_name
    ]
    if len(matching_offsets) != 1:
        raise ValueError(f"audio-profile NV426 requires exactly one {name} mode")
    return matching_offsets[0]


def _playback_mode(arm_modes: bytes, name: str) -> bytes:
    offset = _playback_mode_offset(arm_modes, name)
    return arm_modes[offset : offset + _ARM_MODE_SIZE]


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
    analog_levels = [level & 0xFFFF for level in levels]
    return digital_gain, analog_levels


def _validate_digital_gains(digital_gain: Sequence[int], name: str) -> None:
    """Require seven-bit digital gains while preserving volume levels 1..9."""
    if any(gain > 127 for gain in digital_gain):
        raise ValueError(f"audio-profile {name} digital gain exceeds 127")


def _handsfree_speaker_fields(mode: bytes) -> bytes:
    """Admit speaker-only Handsfree playback and pack its PA and app-0 gains."""
    play_dev_set = struct.unpack_from("<H", mode, _MODE_PLAY_DEV_SET_OFFSET)[0]
    if play_dev_set & _PLAY_ROUTE_MASK != _PLAY_SPEAKER_ONLY:
        message = "audio-profile Handsfree does not select speaker-only playback"
        raise ValueError(message)
    digital_gain, analog_levels = _app0_volume_levels(mode, "Handsfree")
    _validate_digital_gains(digital_gain, "Handsfree")
    if any(analog_levels):
        message = "audio-profile Handsfree PA gain is not the supported zero setting"
        raise ValueError(message)

    pa_word = struct.unpack_from("<H", mode, _MODE_PA_WORD_OFFSET)[0]
    return struct.pack("<H9B", pa_word, *digital_gain)


def _headfree_playback_fields(mode: bytes, *, headphones_only: bool) -> bytes:
    """Pack the admitted Headfree route's PA, headphone PGA and digital gains.

    Combined playback encodes the headphone PGA in bits 7:4 and the PA gain
    in bits 3:0. Headphone-only playback carries the raw headphone PGA.
    """
    play_dev_set = struct.unpack_from("<H", mode, _MODE_PLAY_DEV_SET_OFFSET)[0]
    expected_route = _PLAY_HEADPHONES_ONLY if headphones_only else _PLAY_HEADPHONE_AND_SPEAKER
    if play_dev_set & _PLAY_ROUTE_MASK != expected_route:
        if headphones_only:
            message = "audio-profile Headfree does not select headphone-only playback"
            raise ValueError(message)
        message = "audio-profile Headfree does not select headphone-and-speaker playback"
        raise ValueError(message)
    digital_gain, analog_levels = _app0_volume_levels(mode, "Headfree")
    _validate_digital_gains(digital_gain, "Headfree")
    if len(set(analog_levels)) != 1:
        message = "audio-profile Headfree analog levels 1..9 differ"
        raise ValueError(message)
    analog_level = analog_levels[0]
    if not headphones_only and analog_level >> 8:
        message = "audio-profile Headfree analog level sets bits above the headphone PGA"
        raise ValueError(message)
    if not headphones_only and analog_level & 0x0F:
        message = "audio-profile Headfree PA gain is not the supported zero setting"
        raise ValueError(message)
    headphone_pga = analog_level if headphones_only else analog_level >> 4
    if not 2 <= headphone_pga <= 7:
        message = "audio-profile Headfree headphone PGA level is outside supported range 2..7"
        raise ValueError(message)

    pa_word = struct.unpack_from("<H", mode, _MODE_PA_WORD_OFFSET)[0]
    return struct.pack("<HB9B", pa_word, headphone_pga, *digital_gain)


def _headphone_fed_speaker_fields(
    headset: bytes, handsfree: bytes, headfree: bytes, headphone_pga: int
) -> bytes:
    """Carry headphone-fed speaker gains without integrated-PA settings.

    The compact profile has one headphone PGA for speaker-only playback, so
    all three modes must use that same level. Their digital gains and
    processing remain separate. The original PA words stay in retained NV426;
    the prepared zero words are unused by this topology.
    """
    fields = b""
    for name, mode in (("Headset", headset), ("Handsfree", handsfree), ("Headfree", headfree)):
        play_dev_set = struct.unpack_from("<H", mode, _MODE_PLAY_DEV_SET_OFFSET)[0]
        if play_dev_set & _PLAY_ROUTE_MASK != _PLAY_HEADPHONES_ONLY:
            raise ValueError(f"audio-profile {name} does not select headphone-only codec playback")
        digital_gain, analog_levels = _app0_volume_levels(mode, name)
        _validate_digital_gains(digital_gain, name)
        if any(level != headphone_pga for level in analog_levels):
            raise ValueError(f"audio-profile {name} does not share the Headset headphone PGA")
        if name == "Handsfree":
            fields += struct.pack("<H9B", 0, *digital_gain)
        elif name == "Headfree":
            fields += struct.pack("<HB9B", 0, headphone_pga, *digital_gain)
    return fields


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

    rate_words = [vibrate_tone_words(frequency, rate) for rate in _DAC_SAMPLE_RATES]
    sines = [sine_word for sine_word, _cosine_word in rate_words]
    cosines = [cosine_word for _sine_word, cosine_word in rate_words]
    return struct.pack("<3I3I5H", *sines, *cosines, gain_0, gain_1, gain_down, gain_up, hold)


def _headset_profile_header(
    records: Mapping[int, bytes],
    *,
    machine_compatible: bytes,
    magic: bytes,
) -> tuple[bytes, bytes, int]:
    """Normalize the fitted Headset gains and return its header and mode."""
    if len(machine_compatible) >= 24:
        message = "audio-profile machine compatible must fit its 24-byte terminated field"
        raise ValueError(message)
    mode_count_record = records[425]
    arm_modes = records[426]

    mode_count = mode_count_record[0]
    if mode_count != len(arm_modes) // _ARM_MODE_SIZE:
        message = "audio-profile mode count does not match NV426"
        raise ValueError(message)
    if arm_modes[:16] != b"Headset".ljust(16, b"\0"):
        message = "audio-profile NV426 does not begin with the Headset mode"
        raise ValueError(message)

    headset = arm_modes[:_ARM_MODE_SIZE]
    digital_gain, headphone_pga = _app0_volume_levels(headset, "Headset")
    if len(set(headphone_pga)) != 1:
        message = "audio-profile Headset PGA levels 1..9 differ"
        raise ValueError(message)
    if not 2 <= headphone_pga[0] <= 7:
        message = "audio-profile Headset PGA level is outside supported range 2..7"
        raise ValueError(message)
    _validate_digital_gains(digital_gain, "Headset")

    header = struct.pack(
        "<8s24sB9B",
        magic,
        machine_compatible,
        headphone_pga[0],
        *digital_gain,
    )
    return header, headset, headphone_pga[0]


def prepare_headphone_profile(
    records: Mapping[int, bytes], *, prefix: str, machine_compatible: bytes
) -> PreparedGroup:
    """Normalize only fitted Headset playback for a headphone-only board."""
    profile, headset, _headphone_pga = _headset_profile_header(
        records, machine_compatible=machine_compatible, magic=b"FPAUDHP\0"
    )
    play_dev_set = struct.unpack_from("<H", headset, _MODE_PLAY_DEV_SET_OFFSET)[0]
    if play_dev_set & _PLAY_ROUTE_MASK != _PLAY_HEADPHONES_ONLY:
        message = "audio-profile Headset does not select headphone-only playback"
        raise ValueError(message)
    profile += playback_processing_fields(
        headset, "Headset", records[440], sample_rates=_DAC_SAMPLE_RATES
    )
    return PreparedGroup(
        originals={
            f"{prefix}-nv425.bin": records[425],
            f"{prefix}-nv426.bin": records[426],
            f"{prefix}-nv440.bin": records[440],
        },
        prepared={f"{prefix}-audio-profile.bin": profile},
    )


def _prepare_playback_profile(  # noqa: PLR0913 -- target identity and topology stay explicit.
    records: Mapping[int, bytes],
    *,
    prefix: str,
    machine_compatible: bytes,
    speaker_vibration: bool,
    speaker_from_headphones: bool,
    headfree_headphones_only: bool,
) -> PreparedGroup:
    """Normalize admitted fitted playback modes into the complete kernel input."""
    if speaker_from_headphones and speaker_vibration:
        message = "audio-profile headphone-fed speaker cannot use the integrated-PA vibrate tone"
        raise ValueError(message)
    if speaker_from_headphones and headfree_headphones_only:
        message = "audio-profile headphone-fed speaker cannot use headphone-only Headfree policy"
        raise ValueError(message)
    profile, headset, headphone_pga = _headset_profile_header(
        records, machine_compatible=machine_compatible, magic=b"FPAUDIO\0"
    )
    arm_modes = records[426]
    eq_sets = records[440]
    handsfree = _playback_mode(arm_modes, "Handsfree")
    headfree = _playback_mode(arm_modes, "Headfree")
    if speaker_from_headphones:
        profile += _headphone_fed_speaker_fields(headset, handsfree, headfree, headphone_pga)
    else:
        profile += _handsfree_speaker_fields(handsfree)
        profile += _headfree_playback_fields(headfree, headphones_only=headfree_headphones_only)
    for name, mode in (("Headset", headset), ("Handsfree", handsfree), ("Headfree", headfree)):
        profile += playback_processing_fields(mode, name, eq_sets, sample_rates=_DAC_SAMPLE_RATES)
    if speaker_vibration:
        profile += _handsfree_vibrate_tone_fields(handsfree)
    originals = {
        f"{prefix}-nv425.bin": records[425],
        f"{prefix}-nv426.bin": arm_modes,
        f"{prefix}-nv440.bin": eq_sets,
    }
    return PreparedGroup(
        originals=originals,
        prepared={f"{prefix}-audio-profile.bin": profile},
    )


def prepare_headset_gain_profile(  # noqa: PLR0913 -- target identity and topology stay explicit.
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    prefix: str,
    machine_compatible: bytes,
    speaker_vibration: bool = False,
    speaker_from_headphones: bool = False,
    headfree_headphones_only: bool = False,
) -> PreparedGroup:
    """Admit matching physical fixed-NV playback records before normalization."""
    if downloaded[425] != protected[425]:
        message = "audio-profile mode count differs between DownloadedNV and ProtectNV"
        raise ValueError(message)
    arm_modes = downloaded[426]
    protected_arm_modes = protected[426]
    for name in ("Headset", "Handsfree", "Headfree"):
        offset = _playback_mode_offset(arm_modes, name)
        mode = arm_modes[offset : offset + _ARM_MODE_SIZE]
        if mode != protected_arm_modes[offset : offset + _ARM_MODE_SIZE]:
            raise ValueError(
                f"audio-profile {name} NV426 differs between DownloadedNV and ProtectNV"
            )
        admit_playback_processing(mode, name, downloaded[440], protected[440])
    return _prepare_playback_profile(
        downloaded,
        prefix=prefix,
        machine_compatible=machine_compatible,
        speaker_vibration=speaker_vibration,
        speaker_from_headphones=speaker_from_headphones,
        headfree_headphones_only=headfree_headphones_only,
    )

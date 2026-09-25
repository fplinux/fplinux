# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the compact playback profile from fitted fixed-NV records.

The profile carries the fitted playback gains and, for each playback route,
the stock DAC processing: the VBC EQ6 equalizer sections for every DAC sample
rate, the output scale S6 and the ALC settings.
"""

from __future__ import annotations

import cmath
import math
import struct
from dataclasses import dataclass
from itertools import pairwise
from typing import TYPE_CHECKING

from fplinux_cli.device_data import PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

AUDIO_RECORD_SIZES = {425: 2, 426: 5360, 440: 8160}
_ARM_MODE_SIZE = 1072
_MODE_NAME_SIZE = 16
_MODE_PLAY_DEV_SET_OFFSET = 20
_MODE_APP_COUNT_OFFSET = 36
_MODE_PROCESSING_CONTROL_OFFSET = 40
_MODE_EQ_SWITCH_OFFSET = 44
_MODE_INPUT_GAIN_OFFSET = 46
_MODE_LEVEL_COUNT_OFFSET = 62
_MODE_LEVELS_OFFSET = 68
_MODE_VIBRATE_TONE_OFFSET = 188
_MODE_PA_WORD_OFFSET = 466
_PLAY_ROUTE_MASK = 0x30
_PLAY_SPEAKER_ONLY = 0x20
_PLAY_HEADPHONE_AND_SPEAKER = 0x30
_VIBRATE_TONE_UNIT = 1 << 30
_VIBRATE_TONE_NORM_TOLERANCE = 1e-6
_VIBRATE_TONE_NV_SAMPLE_RATE = 32000
# The kernel selects per-rate data in this order.
_DAC_SAMPLE_RATES = (24000, 32000, 48000)

# Playback processing selection in an NV426 mode and its app-0 entry.
_PLAYBACK_SET_SHIFT = 10
_PLAYBACK_SET_MASK = 0x1F
_EQ_SWITCH_MODE_MASK = 0x000F
_EQ_SWITCH_PLAYER_SELECTS_MODE = 15
_EQ_SWITCH_BYPASS = 0x0010

# One NV440 EQ set. Mode 0 is the pass mode that stock plays without a preset.
_EQ_SET_SIZE = 544
_EQ_SET_NAME_SIZE = 16
_EQ_CONTROL_OFFSET = 16
_EQ_PASS_INPUT_GAIN_OFFSET = 18
_EQ_PASS_BAND_CONTROL_OFFSET = 20
_EQ_PASS_BANDS_OFFSET = 22
_EQ_BAND_SIZE = 8
_EQ_ALC_OFFSET = 522
_EQ_ALC_SIZE = 22
_EQ_CONTROL_EIGHT_BANDS = 0x8000
_EQ_CONTROL_ALC = 0x0100
_EQ_CONTROL_LIMIT_MASK = 0x00FF
_EQ_LIMIT_FULL_SCALE = 0x7F
_BAND_CONTROL_LOW_SHELF = 0x0001
_BAND_CONTROL_HIGH_SHELF = 0x0002
_BAND_CONTROL_LOW_CUT = 0x0080
_BAND_CONTROL_FIRST_BAND = 0x8000
_LOW_CUT_TYPE_SHIFT = 8
_LOW_CUT_TYPE_MASK = 0x7
_LOW_CUT_TWO_SHELVES = 0
_LOW_CUT_BUTTERWORTH = 1
_PEAKING_BAND_COUNT = 5

# Stock filter design: Q12 scales, Q14 coefficients and gains in 0.1 dB.
_INT16_MAX = 0x7FFF
_UNITY_SCALE = 1 << 12
_UNITY_COEFFICIENT = 1 << 14
_GAIN_MIN = -720
_GAIN_MAX = 180
_GAIN_TABLE_UNITY_INDEX = 1440
_QUARTER_TURN_ANGLE = 2048
_HALF_TURN_ANGLE = 4096

# The EQ6 datapath keeps about 6 dB of headroom above a full-scale input. At
# 384 points per octave the sampled peak of the narrowest band that stock can
# design stays within 0.2 dB of its true peak.
_HEADROOM_DB = 6.0
_HEADROOM_LOWEST_FREQUENCY = 20
_HEADROOM_POINTS_PER_OCTAVE = 384

# Stock filter-design tables, generated from their defining formulas. Index i
# of the gain table is the Q12 amplitude of (i - 1440) * 0.05 dB, so a gain in
# 0.1 dB units is two steps and its square root one step. Index i of the
# cosine table is the Q14 cosine of i/8192 of a turn.
LINEAR_GAIN_TABLE = tuple(math.floor(4096 * 10 ** ((index - 1440) / 400)) for index in range(1801))
COSINE_TABLE = tuple(math.floor(16384 * math.cos(index * math.pi / 4096)) for index in range(2049))


@dataclass(frozen=True)
class _Section:
    """One EQ6 biquad in VBC register order.

    The hardware computes (scale / 4096) * (b0 + b1 z^-1 + b2 z^-2) /
    (16384 - minus_a1 z^-1 - minus_a2 z^-2): the feedback registers hold the
    negated denominator terms of the stock design.
    """

    scale: int
    b0: int
    b1: int
    minus_a1: int
    b2: int
    minus_a2: int


_PASS_THROUGH = _Section(
    scale=_UNITY_SCALE, b0=_UNITY_COEFFICIENT, b1=0, minus_a1=0, b2=0, minus_a2=0
)


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

    rate_words = [vibrate_tone_words(frequency, rate) for rate in _DAC_SAMPLE_RATES]
    sines = [sine_word for sine_word, _cosine_word in rate_words]
    cosines = [cosine_word for _sine_word, cosine_word in rate_words]
    return struct.pack("<3I3I5H", *sines, *cosines, gain_0, gain_1, gain_down, gain_up, hold)


def _c_divide(numerator: int, denominator: int) -> int:
    """Divide integers as C does, truncating the quotient toward zero."""
    quotient = abs(numerator) // abs(denominator)
    return quotient if (numerator < 0) == (denominator < 0) else -quotient


def _int16(value: int) -> int:
    """Wrap an integer as the stock firmware does when it stores it in an int16."""
    return (value + 0x8000) % 0x10000 - 0x8000


def _gain_in_range(gain: int) -> bool:
    """Return whether a gain in 0.1 dB lies in the stock -72..+18 dB range."""
    return _GAIN_MIN <= gain <= _GAIN_MAX


def _table_gain(gain: int) -> int:
    """Return the Q12 amplitude of an in-range gain given in 0.1 dB."""
    return LINEAR_GAIN_TABLE[_GAIN_TABLE_UNITY_INDEX + 2 * gain]


def _table_root_gain(gain: int) -> int:
    """Return the Q12 square root of the amplitude of an in-range gain given in 0.1 dB."""
    return LINEAR_GAIN_TABLE[_GAIN_TABLE_UNITY_INDEX + gain]


def _frequency_angle(frequency: int, sample_rate: int) -> int:
    """Return the per-sample phase step of a frequency in 1/8192 of a turn."""
    return _int16(_c_divide(frequency << 13, sample_rate))


def _half_angle_tangent(angle: int) -> int:
    """Return the Q12 tangent of half a non-negative angle below a full turn."""
    half = _c_divide(angle, 2)
    return _c_divide(COSINE_TABLE[_QUARTER_TURN_ANGLE - half] << 12, COSINE_TABLE[half])


def _stored_section(
    scale: int, *, numerator: tuple[int, int, int], denominator: tuple[int, int]
) -> _Section:
    """Fit a stock design into the 16-bit coefficient registers.

    The design is (scale / 4096) * (b0 + b1 z^-1 + b2 z^-2) /
    (16384 + a1 z^-1 + a2 z^-2). Stock halves the numerator until it fits in
    16 bits and moves that gain into the section scale, then stores every
    term as int16 and negates the denominator terms for the feedback registers.
    """
    b0, b1, b2 = numerator
    a1, a2 = denominator
    shift = 0
    largest = max(abs(b0), abs(b1), abs(b2))
    while largest > _INT16_MAX:
        largest //= 2
        shift += 1
    if shift:
        b0, b1, b2 = b0 >> shift, b1 >> shift, b2 >> shift
        scale = min(scale << shift, _INT16_MAX)
    return _Section(
        scale=scale,
        b0=_int16(b0),
        b1=_int16(b1),
        minus_a1=_int16(-_int16(a1)),
        b2=_int16(b2),
        minus_a2=_int16(-_int16(a2)),
    )


def _peaking_section(band: Sequence[int], sample_rate: int) -> _Section:
    """Design one enabled peaking band as the stock firmware does.

    A band holds its centre frequency in Hz, Q times 512, and its boost and
    base gain in 0.1 dB. The stock integer arithmetic is kept, including
    truncating division and int16 intermediates. Every band that stock cannot
    design -- a centre frequency that is not between 0 and fs/2 or a gain
    outside -72..+18 dB -- passes through, as the stock caller substitutes a
    pass-through section. A zero boost leaves only the base gain.
    """
    frequency, q, boost, base_gain = band
    if not 0 < frequency < sample_rate >> 1:
        return _PASS_THROUGH
    # This minimum Q keeps the Q12 bandwidth tangent within 16 bits.
    minimum_q = _int16(_c_divide(1113 * frequency, sample_rate) + 1)
    bandwidth = _int16(_c_divide(frequency << 9, max(q, minimum_q)))
    bandwidth_angle = _frequency_angle(bandwidth, sample_rate)
    centre_angle = _frequency_angle(frequency, sample_rate)
    if not _gain_in_range(boost) or not _gain_in_range(base_gain):
        return _PASS_THROUGH
    if _c_divide(bandwidth_angle, 2) >= _QUARTER_TURN_ANGLE:
        return _PASS_THROUGH
    base_scale = _table_gain(base_gain)
    if boost == 0:
        return _stored_section(
            base_scale, numerator=(_UNITY_COEFFICIENT, 0, 0), denominator=(0, 0)
        )

    root_gain = _table_root_gain(boost)
    tangent = _int16(_half_angle_tangent(bandwidth_angle))
    divisor = root_gain + tangent
    edge_term = _c_divide(_UNITY_COEFFICIENT * root_gain, divisor)
    peak_term = (_c_divide(tangent * root_gain, divisor) * root_gain) >> 10
    # Below fs/2 the centre angle stays below half a turn.
    if centre_angle <= _QUARTER_TURN_ANGLE:
        cosine = COSINE_TABLE[centre_angle]
    else:
        cosine = -COSINE_TABLE[_HALF_TURN_ANGLE - centre_angle]
    b1 = _c_divide(-((cosine * root_gain) << 1), divisor)
    a2 = _c_divide((root_gain - tangent) << 14, divisor)
    return _stored_section(
        base_scale,
        numerator=(edge_term + peak_term, b1, edge_term - peak_term),
        denominator=(b1, a2),
    )


def _low_cut_section(parameters: Sequence[int], sample_rate: int) -> _Section:
    """Design the enabled two-shelf low-cut filter as the stock firmware does.

    Each of the two first-order shelves moves from its DC gain to its
    high-frequency gain around a corner frequency; gains are in 0.1 dB and
    corners in non-negative Hz. Stock passes a gain outside -72..+18 dB or a
    corner at or above fs/2 through.
    """
    first_dc, first_high, first_corner, second_dc, second_high, second_corner = parameters
    if not all(map(_gain_in_range, (first_dc, first_high, second_dc, second_high))):
        return _PASS_THROUGH
    first_angle = _frequency_angle(first_corner, sample_rate)
    second_angle = _frequency_angle(second_corner, sample_rate)
    if max(_c_divide(first_angle, 2), _c_divide(second_angle, 2)) >= _QUARTER_TURN_ANGLE:
        return _PASS_THROUGH

    first_tangent = _half_angle_tangent(first_angle)
    second_tangent = _half_angle_tangent(second_angle)
    first_dc_term = (_table_gain(first_dc) * first_tangent) >> 12
    second_dc_term = (_table_gain(second_dc) * second_tangent) >> 12
    first_high_gain = _table_gain(first_high)
    second_high_gain = _table_gain(second_high)
    # Bilinear-transform numerator and denominator in Q24.
    b0 = first_high_gain * second_high_gain
    b1 = first_dc_term * second_high_gain + second_dc_term * first_high_gain
    b2 = first_dc_term * second_dc_term
    a0 = 1 << 24
    a1 = (first_tangent + second_tangent) << 12
    a2 = first_tangent * second_tangent
    divisor = (a0 + a1 + a2) >> 12

    def rounded_q14(value: int, shift: int) -> int:
        """Normalize by the denominator sum and round half up to Q14."""
        return (_c_divide(value << shift, divisor) + 1) >> 1

    return _stored_section(
        _UNITY_SCALE,
        numerator=(
            rounded_q14(b0 + b1 + b2, 3),
            rounded_q14(b2 - b0, 4),
            rounded_q14(b0 - b1 + b2, 3),
        ),
        denominator=(rounded_q14(a2 - a0, 4), rounded_q14(a0 - a1 + a2, 3)),
    )


def _pass_mode_bands(eq_set: bytes) -> tuple[int, list[tuple[int, ...]]]:
    """Return the pass-mode band control word and its seven band records."""
    band_control = struct.unpack_from("<H", eq_set, _EQ_PASS_BAND_CONTROL_OFFSET)[0]
    bands = [
        struct.unpack_from("<4h", eq_set, _EQ_PASS_BANDS_OFFSET + index * _EQ_BAND_SIZE)
        for index in range(7)
    ]
    return band_control, bands


def _low_cut_parameters(bands: Sequence[Sequence[int]]) -> tuple[int, ...]:
    """Return the two-shelf low-cut gains and corners stored in pass-mode bands 5 and 6."""
    return (*bands[5], *bands[6][:2])


def _pass_mode_sections(eq_set: bytes, sample_rate: int) -> list[_Section]:
    """Design the six EQ6 sections of an EQ set's pass mode for one DAC sample rate.

    Section 0 is the low-cut filter and sections 1..5 are bands 0..4. As in
    stock, a disabled filter, a band marked as a shelf and a low-cut type for
    which stock has no design pass through. The caller has already rejected
    the Butterworth low-cut type.
    """
    band_control, bands = _pass_mode_bands(eq_set)
    low_cut_type = band_control >> _LOW_CUT_TYPE_SHIFT & _LOW_CUT_TYPE_MASK
    low_cut = _PASS_THROUGH
    if band_control & _BAND_CONTROL_LOW_CUT and low_cut_type == _LOW_CUT_TWO_SHELVES:
        low_cut = _low_cut_section(_low_cut_parameters(bands), sample_rate)

    sections = [low_cut]
    for index in range(_PEAKING_BAND_COUNT):
        enabled = band_control & (_BAND_CONTROL_FIRST_BAND >> index)
        low_shelf = index == 0 and band_control & _BAND_CONTROL_LOW_SHELF
        high_shelf = index == _PEAKING_BAND_COUNT - 1 and band_control & _BAND_CONTROL_HIGH_SHELF
        if enabled and not low_shelf and not high_shelf:
            sections.append(_peaking_section(bands[index], sample_rate))
        else:
            sections.append(_PASS_THROUGH)
    return sections


def _is_stable(section: _Section) -> bool:
    """Return whether the quantized denominator has both poles inside the unit circle."""
    return (
        abs(section.minus_a2) < _UNITY_COEFFICIENT
        and abs(section.minus_a1) < _UNITY_COEFFICIENT - section.minus_a2
    )


def _peak_running_gain_db(sections: Sequence[_Section], sample_rate: int) -> float:
    """Return the largest gain after any section between 20 Hz and fs/2."""
    nyquist = sample_rate / 2
    octaves = math.log2(nyquist / _HEADROOM_LOWEST_FREQUENCY)
    point_count = math.ceil(octaves * _HEADROOM_POINTS_PER_OCTAVE)
    peak = 0.0
    for point in range(point_count + 1):
        frequency = _HEADROOM_LOWEST_FREQUENCY * 2 ** (octaves * point / point_count)
        delay = cmath.exp(-2j * math.pi * frequency / sample_rate)
        response: complex = 1
        for section in sections:
            numerator = section.b0 + (section.b1 + section.b2 * delay) * delay
            denominator = (
                _UNITY_COEFFICIENT - (section.minus_a1 + section.minus_a2 * delay) * delay
            )
            response *= section.scale / _UNITY_SCALE * numerator / denominator
            peak = max(peak, abs(response))
    return 20 * math.log10(peak)


def _checked_pass_mode_sections(eq_set: bytes, set_name: str, sample_rate: int) -> list[_Section]:
    """Design one rate's sections and reject an unstable or overflowing result."""
    sections = _pass_mode_sections(eq_set, sample_rate)
    for index, section in enumerate(sections):
        if not _is_stable(section):
            raise ValueError(
                f"audio-profile {set_name} section {index} is unstable at {sample_rate} Hz"
            )
    peak_gain = _peak_running_gain_db(sections, sample_rate)
    if peak_gain > _HEADROOM_DB:
        raise ValueError(
            f"audio-profile {set_name} boosts {sample_rate} Hz playback by {peak_gain:.1f} dB, "
            f"above the {_HEADROOM_DB:.0f} dB equalizer headroom"
        )
    return sections


def _selected_eq_set(
    mode: bytes, name: str, eq_sets: bytes, protected_eq_sets: bytes
) -> tuple[bytes, str]:
    """Return the NV440 EQ set that a playback mode selects, and its name.

    Bits 14:10 of the mode's first processing-control word select the set that
    stock applies to all playback other than MIDI and AMR.
    """
    control = struct.unpack_from("<H", mode, _MODE_PROCESSING_CONTROL_OFFSET)[0]
    index = control >> _PLAYBACK_SET_SHIFT & _PLAYBACK_SET_MASK
    set_count = len(eq_sets) // _EQ_SET_SIZE
    if not 1 <= index <= set_count:
        raise ValueError(f"audio-profile {name} selects EQ set {index}; expected 1..{set_count}")
    set_name = f"EQ_{name}"
    offset = (index - 1) * _EQ_SET_SIZE
    eq_set = eq_sets[offset : offset + _EQ_SET_SIZE]
    if eq_set[:_EQ_SET_NAME_SIZE] != set_name.encode().ljust(_EQ_SET_NAME_SIZE, b"\0"):
        raise ValueError(f"audio-profile {name} selects EQ set {index}, which is not {set_name}")
    if eq_set != protected_eq_sets[offset : offset + _EQ_SET_SIZE]:
        raise ValueError(
            f"audio-profile {set_name} NV440 differs between DownloadedNV and ProtectNV"
        )
    return eq_set, set_name


def _playback_processing_fields(
    mode: bytes, name: str, eq_sets: bytes, protected_eq_sets: bytes
) -> bytes:
    """Design the stock playback processing of one mode for every DAC sample rate.

    The fields are the output scale S6, the ALC enable and its eleven register
    words, then six EQ6 sections of six int16 words for each DAC sample rate.
    Only the player-selected pass mode with the full-scale output limit, five
    bands and no Butterworth low-cut filter is admitted.
    """
    eq_switch = struct.unpack_from("<H", mode, _MODE_EQ_SWITCH_OFFSET)[0]
    if eq_switch & _EQ_SWITCH_BYPASS:
        message = f"audio-profile {name} bypasses playback processing, which is not supported"
        raise ValueError(message)
    if eq_switch & _EQ_SWITCH_MODE_MASK != _EQ_SWITCH_PLAYER_SELECTS_MODE:
        message = f"audio-profile {name} equalizer mode is not selected by the player"
        raise ValueError(message)
    eq_set, set_name = _selected_eq_set(mode, name, eq_sets, protected_eq_sets)

    eq_control = struct.unpack_from("<H", eq_set, _EQ_CONTROL_OFFSET)[0]
    if eq_control & _EQ_CONTROL_EIGHT_BANDS:
        message = f"audio-profile {set_name} uses eight bands; only five are supported"
        raise ValueError(message)
    if eq_control & _EQ_CONTROL_LIMIT_MASK != _EQ_LIMIT_FULL_SCALE:
        message = f"audio-profile {set_name} output limit is not the supported full-scale setting"
        raise ValueError(message)
    band_control, bands = _pass_mode_bands(eq_set)
    if band_control & _BAND_CONTROL_LOW_CUT:
        low_cut_type = band_control >> _LOW_CUT_TYPE_SHIFT & _LOW_CUT_TYPE_MASK
        parameters = _low_cut_parameters(bands)
        first_corner, second_corner = parameters[2], parameters[5]
        # A negative corner has no stock meaning: it would index outside the cosine table.
        if low_cut_type == _LOW_CUT_TWO_SHELVES and min(first_corner, second_corner) < 0:
            message = f"audio-profile {set_name} low-cut corner frequency is negative"
            raise ValueError(message)
        if low_cut_type == _LOW_CUT_BUTTERWORTH:
            message = f"audio-profile {set_name} Butterworth low-cut filter is not supported"
            raise ValueError(message)

    mode_input_gain = struct.unpack_from("<h", mode, _MODE_INPUT_GAIN_OFFSET)[0]
    set_input_gain = struct.unpack_from("<h", eq_set, _EQ_PASS_INPUT_GAIN_OFFSET)[0]
    output_scale = min((mode_input_gain * set_input_gain) >> 12, _INT16_MAX)
    if output_scale <= 0:
        message = f"audio-profile {name} output scale {output_scale} is not positive"
        raise ValueError(message)

    alc_enabled = bool(eq_control & _EQ_CONTROL_ALC)
    fields = struct.pack("<HB", output_scale, alc_enabled)
    fields += eq_set[_EQ_ALC_OFFSET : _EQ_ALC_OFFSET + _EQ_ALC_SIZE]
    for sample_rate in _DAC_SAMPLE_RATES:
        for section in _checked_pass_mode_sections(eq_set, set_name, sample_rate):
            fields += struct.pack(
                "<6h",
                section.scale,
                section.b0,
                section.b1,
                section.minus_a1,
                section.b2,
                section.minus_a2,
            )
    return fields


def prepare_headset_gain_profile(
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    prefix: str,
    machine_compatible: bytes,
    speaker_vibration: bool = False,
) -> PreparedGroup:
    """Normalize fitted playback gains and processing into the compact kernel input.

    The processing section follows the gains, in Headset, Handsfree and
    Headfree order. ``speaker_vibration`` appends the Handsfree vibrate tone
    for phones whose speaker is also the vibration actuator.
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

    profile = struct.pack(
        "<8s24sB9B",
        b"FPAUDIO\0",
        machine_compatible,
        headphone_pga[0],
        *digital_gain,
    )
    headset = arm_modes[:_ARM_MODE_SIZE]
    handsfree = _admitted_mode(arm_modes, protected_arm_modes, "Handsfree")
    headfree = _admitted_mode(arm_modes, protected_arm_modes, "Headfree")
    profile += _handsfree_speaker_fields(handsfree)
    profile += _headfree_combined_fields(headfree)
    for name, mode in (("Headset", headset), ("Handsfree", handsfree), ("Headfree", headfree)):
        profile += _playback_processing_fields(mode, name, eq_sets, protected_eq_sets)
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

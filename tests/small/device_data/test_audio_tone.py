# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data audio tone scenarios."""

from __future__ import annotations

import struct

import pytest
from fplinux_cli.device_data import audio_profile

from tests.small.device_data.audio_fixtures import (
    INOI_HANDSFREE_VIBRATE_TONE_OFFSET,
    inoi_audio_records,
)


class SpeakerVibrateToneTests:
    """Protect the vibrate-tone words derived from fitted Handsfree NV data."""

    @pytest.mark.parametrize(
        ("frequency", "sample_rate", "sine", "cosine"),
        [
            pytest.param(157, 32000, 0xFE07032E, 0x3FF8373E, id="ap-table-32-khz"),
            pytest.param(175, 44100, 0xFE67891A, 0x3FFAE856, id="ap-table-44_1-khz"),
            pytest.param(180, 48000, 0xFE7DFF2F, 0x3FFB73CA, id="ap-table-48-khz"),
            pytest.param(175, 32000, 0xFDCD232B, 0x3FF65428, id="nv-handsfree"),
            pytest.param(150, 32000, 0xFE1D8569, 0x3FF8E4F7, id="nv-headfree"),
        ],
    )
    def test_tone_words_reproduce_stock_firmware_coefficient_tables(
        self, *, frequency: int, sample_rate: int, sine: int, cosine: int
    ) -> None:
        """Stock AP tables and fitted NV pairs are the words for their tone and rate."""
        assert audio_profile.vibrate_tone_words(frequency, sample_rate) == (sine, cosine)

    @pytest.mark.parametrize(
        ("words", "error"),
        [
            pytest.param(
                (0xFDCD, 0x232B, 0x3FF6, 0x5428, 0, 0x82, 2, 8, 0x205),
                "gain is zero",
                id="zero-gain-0",
            ),
            pytest.param(
                (0xFDCD, 0x232B, 0x3FF6, 0x5428, 0x82, 0, 2, 8, 0x205),
                "gain is zero",
                id="zero-gain-1",
            ),
            pytest.param(
                (0xFDCD, 0x232B, 0x3FF6, 0x5428, 0x82, 0x82, 2, 8, 0),
                "hold is zero",
                id="zero-hold",
            ),
            pytest.param(
                (0, 0, 0, 0, 0x82, 0x82, 2, 8, 0x205),
                "not a unit rotation",
                id="unprogrammed-pair",
            ),
            pytest.param(
                (0xFEE6, 0x9195, 0x1FFB, 0x2A14, 0x82, 0x82, 2, 8, 0x205),
                "not a unit rotation",
                id="half-scale-pair",
            ),
            pytest.param(
                (0, 0, 0x4000, 0, 0x82, 0x82, 2, 8, 0x205),
                "0.00 Hz is not between 0 and 16000 Hz",
                id="zero-frequency",
            ),
            pytest.param(
                (0x0232, 0xDCD5, 0x3FF6, 0x5428, 0x82, 0x82, 2, 8, 0x205),
                "-175.00 Hz is not between 0 and 16000 Hz",
                id="negative-frequency",
            ),
            pytest.param(
                (0xFDCD, 0x232B, 0x3FF6, 0x5438, 0x82, 0x82, 2, 8, 0x205),
                "not reproducible at 32000 Hz",
                id="cosine-off-by-16",
            ),
        ],
    )
    def test_profile_rejects_silent_out_of_band_or_inexact_tone(
        self, *, words: tuple[int, ...], error: str
    ) -> None:
        """Only an audible, exactly reproducible fitted tone can drive the speaker."""
        downloaded, protected = inoi_audio_records()
        for records in (downloaded, protected):
            arm = bytearray(records[426])
            struct.pack_into("<9H", arm, INOI_HANDSFREE_VIBRATE_TONE_OFFSET, *words)
            records[426] = bytes(arm)
        with pytest.raises(ValueError, match=error):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="inoi240",
                machine_compatible=b"inoi,240-modern-4g",
                speaker_vibration=True,
            )

    def test_profile_rejects_tone_that_differs_between_nv_copies(self) -> None:
        """ProtectNV must confirm the tone words, not only the playback gains."""
        downloaded, protected = inoi_audio_records()
        arm = bytearray(protected[426])
        struct.pack_into("<H", arm, INOI_HANDSFREE_VIBRATE_TONE_OFFSET + 16, 0x0206)
        protected[426] = bytes(arm)

        with pytest.raises(ValueError, match="Handsfree NV426 differs"):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="inoi240",
                machine_compatible=b"inoi,240-modern-4g",
                speaker_vibration=True,
            )

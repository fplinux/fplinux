# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data audio tone scenarios."""

from __future__ import annotations

import struct
import unittest

from fplinux_cli.device_data import audio_profile

from tests.small.device_data.audio_fixtures import (
    INOI_HANDSFREE_VIBRATE_TONE_OFFSET,
    inoi_audio_records,
)


class SpeakerVibrateToneTests(unittest.TestCase):
    """Protect the vibrate-tone words derived from fitted Handsfree NV data."""

    def test_tone_words_reproduce_stock_firmware_coefficient_tables(self) -> None:
        """Stock AP tables and fitted NV pairs are the words for their tone and rate."""
        cases = (
            ("AP table 32 kHz", 157, 32000, 0xFE07032E, 0x3FF8373E),
            ("AP table 44.1 kHz", 175, 44100, 0xFE67891A, 0x3FFAE856),
            ("AP table 48 kHz", 180, 48000, 0xFE7DFF2F, 0x3FFB73CA),
            ("NV Handsfree", 175, 32000, 0xFDCD232B, 0x3FF65428),
            ("NV Headfree", 150, 32000, 0xFE1D8569, 0x3FF8E4F7),
        )
        for name, frequency, sample_rate, sine, cosine in cases:
            with self.subTest(name=name):
                self.assertEqual(
                    audio_profile.vibrate_tone_words(frequency, sample_rate), (sine, cosine)
                )

    def test_profile_rejects_silent_out_of_band_or_inexact_tone(self) -> None:
        """Only an audible, exactly reproducible fitted tone can drive the speaker."""
        cases = (
            (
                "zero gain 0",
                (0xFDCD, 0x232B, 0x3FF6, 0x5428, 0, 0x82, 2, 8, 0x205),
                "gain is zero",
            ),
            (
                "zero gain 1",
                (0xFDCD, 0x232B, 0x3FF6, 0x5428, 0x82, 0, 2, 8, 0x205),
                "gain is zero",
            ),
            ("zero hold", (0xFDCD, 0x232B, 0x3FF6, 0x5428, 0x82, 0x82, 2, 8, 0), "hold is zero"),
            ("unprogrammed pair", (0, 0, 0, 0, 0x82, 0x82, 2, 8, 0x205), "not a unit rotation"),
            (
                "half-scale pair",
                (0xFEE6, 0x9195, 0x1FFB, 0x2A14, 0x82, 0x82, 2, 8, 0x205),
                "not a unit rotation",
            ),
            (
                "zero frequency",
                (0, 0, 0x4000, 0, 0x82, 0x82, 2, 8, 0x205),
                "0.00 Hz is not between 0 and 16000 Hz",
            ),
            (
                "negative frequency",
                (0x0232, 0xDCD5, 0x3FF6, 0x5428, 0x82, 0x82, 2, 8, 0x205),
                "-175.00 Hz is not between 0 and 16000 Hz",
            ),
            (
                "cosine off by 16",
                (0xFDCD, 0x232B, 0x3FF6, 0x5438, 0x82, 0x82, 2, 8, 0x205),
                "not reproducible at 32000 Hz",
            ),
        )
        for name, words, error in cases:
            downloaded, protected = inoi_audio_records()
            for records in (downloaded, protected):
                arm = bytearray(records[426])
                struct.pack_into("<9H", arm, INOI_HANDSFREE_VIBRATE_TONE_OFFSET, *words)
                records[426] = bytes(arm)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
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

        with self.assertRaisesRegex(ValueError, "Handsfree NV426 differs"):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="inoi240",
                machine_compatible=b"inoi,240-modern-4g",
                speaker_vibration=True,
            )


if __name__ == "__main__":
    unittest.main()

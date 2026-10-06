# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data audio profiles scenarios."""

from __future__ import annotations

import struct
import unittest

from fplinux_cli.device_data import audio_profile

from tests.small.device_data.audio_expected import (
    FITTED_INOI_PROCESSING_SECTION,
    FITTED_NOKIA_PROCESSING_SECTION,
    INOI_VIBRATE_TONE_SECTION,
)
from tests.small.device_data.audio_fixtures import (
    INOI_EQ_SETS,
    headset_audio_records,
    inoi_audio_records,
    nokia_audio_records,
)


class HeadsetGainProfileTests(unittest.TestCase):
    """Protect the compact kernel input and admitted fitted playback gains."""

    def test_fitted_playback_modes_become_each_exact_profile(self) -> None:
        """Fitted records and the given flags become the literal compact payload of each size.

        Each case passes machine_compatible and speaker_vibration itself; the target
        parsers that choose these values for a phone are not executed here.
        """
        cases = (
            (
                "inoi240",
                b"inoi,240-modern-4g",
                inoi_audio_records,
                True,
                (
                    b"FPAUDIO\0"
                    b"inoi,240-modern-4g\0\0\0\0\0\0"
                    b"\x07\x6c\x61\x56\x4b\x41\x37\x2d\x23\x1b"
                    b"\x1a\x00\x39\x35\x31\x2d\x29\x21\x1d\x19\x17"
                    b"\x1a\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a"
                )
                + FITTED_INOI_PROCESSING_SECTION
                + INOI_VIBRATE_TONE_SECTION,
                822,
            ),
            (
                "inoi244",
                b"inoi,244-modern-4g",
                inoi_audio_records,
                True,
                (
                    b"FPAUDIO\0"
                    b"inoi,244-modern-4g\0\0\0\0\0\0"
                    b"\x07\x6c\x61\x56\x4b\x41\x37\x2d\x23\x1b"
                    b"\x1a\x00\x39\x35\x31\x2d\x29\x21\x1d\x19\x17"
                    b"\x1a\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a"
                )
                + FITTED_INOI_PROCESSING_SECTION
                + INOI_VIBRATE_TONE_SECTION,
                822,
            ),
            (
                "ta1618",
                b"nokia,ta-1618",
                nokia_audio_records,
                False,
                (
                    b"FPAUDIO\0"
                    b"nokia,ta-1618\0\0\0\0\0\0\0\0\0\0\0"
                    b"\x06\x47\x42\x3d\x37\x32\x2c\x26\x20\x1a"
                    b"\x1a\x00\x38\x34\x30\x2c\x28\x24\x20\x1c\x1b"
                    b"\x1a\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a"
                )
                + FITTED_NOKIA_PROCESSING_SECTION,
                788,
            ),
        )
        for prefix, compatible, records, speaker_vibration, expected, size in cases:
            with self.subTest(prefix=prefix):
                downloaded, protected = records()

                result = audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix=prefix,
                    machine_compatible=compatible,
                    speaker_vibration=speaker_vibration,
                )

                self.assertEqual(result.prepared, {f"{prefix}-audio-profile.bin": expected})
                self.assertEqual(len(expected), size)
                self.assertEqual(result.originals[f"{prefix}-nv425.bin"], downloaded[425])
                self.assertEqual(result.originals[f"{prefix}-nv426.bin"], downloaded[426])
                self.assertEqual(result.originals[f"{prefix}-nv440.bin"], downloaded[440])

    def test_speaker_profiles_select_handsfree_and_headfree_by_name_after_reordering(
        self,
    ) -> None:
        """Speaker and processing sections come from named modes, not fixed mode indexes."""
        downloaded, protected = nokia_audio_records()
        for records in (downloaded, protected):
            arm = bytearray(records[426])
            struct.pack_into("<H", arm, 1072 + 466, 0x0006)
            headfree = bytes(arm[1072 : 2 * 1072])
            handsfree = bytes(arm[3 * 1072 : 4 * 1072])
            arm[1072 : 2 * 1072] = handsfree
            arm[3 * 1072 : 4 * 1072] = headfree
            records[426] = bytes(arm)

        result = audio_profile.prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="ta1618",
            machine_compatible=b"nokia,ta-1618",
        )

        self.assertEqual(
            result.prepared["ta1618-audio-profile.bin"][42:],
            b"\x1a\x00\x38\x34\x30\x2c\x28\x24\x20\x1c\x1b"
            b"\x06\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a" + FITTED_NOKIA_PROCESSING_SECTION,
        )

    def test_speaker_profile_rejects_wrong_route_or_gain_or_disagreeing_copies(self) -> None:
        """Only matching speaker-only Handsfree calibration can enable its output."""
        cases = (
            ("different copy", 3 * 1072 + 466, b"\x1b\x00", False, "Handsfree NV426 differs"),
            ("combined route", 3 * 1072 + 20, b"\x32\x00", True, "speaker-only"),
            ("wrong level count", 3 * 1072 + 62, b"\x08\x00", True, "expected 9"),
            ("analog gain", 3 * 1072 + 68, b"\x01\x00", True, "PA gain"),
            ("oversized digital gain", 3 * 1072 + 70, b"\x80\x00", True, "exceeds 127"),
            ("increasing digital gain", 3 * 1072 + 74, b"\x39\x00", True, "not monotonically"),
        )
        for name, offset, replacement, change_both, error in cases:
            downloaded, protected = nokia_audio_records()
            copies = (downloaded, protected) if change_both else (protected,)
            for records in copies:
                arm = bytearray(records[426])
                arm[offset : offset + len(replacement)] = replacement
                records[426] = bytes(arm)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix="ta1618",
                    machine_compatible=b"nokia,ta-1618",
                )

    def test_combined_profile_rejects_wrong_route_or_gain_or_disagreeing_copies(self) -> None:
        """Only one matching headphone-and-speaker Headfree calibration can drive both outputs."""

        def every_level(analog: bytes) -> dict[int, bytes]:
            """Replace the analog half of all nine Headfree volume-level words."""
            return {1072 + 68 + 4 * level: analog for level in range(9)}

        cases: tuple[tuple[str, dict[int, bytes], bool, str], ...] = (
            ("different copy", {1072 + 466: b"\x1b\x00"}, False, "Headfree NV426 differs"),
            ("absent mode", {1072: b"Headphone"}, True, "exactly one Headfree mode"),
            ("duplicate mode", {4 * 1072: b"Headfree"}, True, "exactly one Headfree mode"),
            ("speaker-only route", {1072 + 20: b"\x22\x00"}, True, "Headfree does not select"),
            ("wrong level count", {1072 + 62: b"\x08\x00"}, True, "Headfree app 0 has 8"),
            ("increasing digital gain", {1072 + 74: b"\x4f\x00"}, True, "Headfree digital"),
            ("one analog level differs", {1072 + 100: b"\x60\x00"}, True, "analog levels 1..9"),
            ("nonzero PA gain", every_level(b"\x71\x00"), True, "Headfree PA gain"),
            ("headphone PGA 1", every_level(b"\x10\x00"), True, "Headfree headphone PGA"),
            ("headphone PGA 8", every_level(b"\x80\x00"), True, "Headfree headphone PGA"),
            ("bits above PGA", every_level(b"\x70\x01"), True, "Headfree analog level sets"),
        )
        for name, replacements, change_both, error in cases:
            downloaded, protected = nokia_audio_records()
            copies = (downloaded, protected) if change_both else (protected,)
            for records in copies:
                arm = bytearray(records[426])
                for offset, replacement in replacements.items():
                    arm[offset : offset + len(replacement)] = replacement
                records[426] = bytes(arm)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix="ta1618",
                    machine_compatible=b"nokia,ta-1618",
                )

    def test_profile_rejects_unsafe_headset_values(self) -> None:
        """A fitted profile is emitted only from matching, bounded Headset data."""
        # Both NV426 copies receive each change, so the copies still agree.
        cases: dict[str, tuple[int, bytes, str]] = {
            "wrong level count": (62, b"\x08\x00", "expected 9"),
            "different PGA": (68, b"\x06\x00", "PGA levels 1..9 differ"),
            "oversized digital gain": (70, b"\x80\x00", "exceeds 127"),
            "increasing digital gain": (74, b"\x6d\x00", "not monotonically"),
        }
        for name, (offset, replacement, error) in cases.items():
            downloaded, protected = inoi_audio_records()
            for records in (downloaded, protected):
                changed = bytearray(records[426])
                changed[offset : offset + len(replacement)] = replacement
                records[426] = bytes(changed)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix="phone",
                    machine_compatible=b"vendor,phone",
                )

        downloaded, protected = headset_audio_records(
            (
                0x006C0008,
                0x00610008,
                0x00560008,
                0x004B0008,
                0x00410008,
                0x00370008,
                0x002D0008,
                0x00230008,
                0x001B0008,
            ),
            input_gain=0x071D,
            eq_sets=INOI_EQ_SETS,
        )
        with self.assertRaisesRegex(ValueError, "outside supported range 2..7"):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="phone",
                machine_compatible=b"vendor,phone",
            )

    def test_profile_rejects_nv_copies_that_disagree_on_carried_values(self) -> None:
        """DownloadedNV and ProtectNV must agree on every value carried into the profile."""
        cases = (
            (426, 68, b"\x06\x00", "Headset NV426 differs"),
            (426, 70, b"\x6b\x00", "Headset NV426 differs"),
            (440, 20, b"\x01\x00", "EQ_Headset NV440 differs"),
            (440, 3 * 544 + 22, b"\xab\x00", "EQ_Handsfree NV440 differs"),
        )
        for identifier, offset, replacement, error in cases:
            downloaded, protected = inoi_audio_records()
            changed = bytearray(protected[identifier])
            changed[offset : offset + len(replacement)] = replacement
            protected[identifier] = bytes(changed)
            with (
                self.subTest(identifier=identifier, offset=offset),
                self.assertRaisesRegex(ValueError, error),
            ):
                audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix="phone",
                    machine_compatible=b"vendor,phone",
                )


if __name__ == "__main__":
    unittest.main()

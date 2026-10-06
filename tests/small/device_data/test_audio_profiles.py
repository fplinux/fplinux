# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data audio profiles scenarios."""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

import pytest
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

if TYPE_CHECKING:
    from collections.abc import Callable


def _every_headfree_level(analog: bytes) -> dict[int, bytes]:
    """Replace the analog half of all nine Headfree volume-level words."""
    return {1072 + 68 + 4 * level: analog for level in range(9)}


class HeadsetGainProfileTests:
    """Protect the compact kernel input and admitted fitted playback gains."""

    @pytest.mark.parametrize(
        ("prefix", "compatible", "record_factory", "speaker_vibration", "expected", "size"),
        [
            pytest.param(
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
                id="inoi240",
            ),
            pytest.param(
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
                id="inoi244",
            ),
            pytest.param(
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
                id="ta1618",
            ),
        ],
    )
    # Profile inputs, expected bytes and size are independent parameterized case dimensions.
    def test_fitted_playback_modes_become_each_exact_profile(  # noqa: PLR0913
        self,
        *,
        prefix: str,
        compatible: bytes,
        record_factory: Callable[[], tuple[dict[int, bytes], dict[int, bytes]]],
        speaker_vibration: bool,
        expected: bytes,
        size: int,
    ) -> None:
        """Fitted records and the given flags become the literal compact payload of each size.

        Each case passes machine_compatible and speaker_vibration itself; the target
        parsers that choose these values for a phone are not executed here.
        """
        downloaded, protected = record_factory()

        result = audio_profile.prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix=prefix,
            machine_compatible=compatible,
            speaker_vibration=speaker_vibration,
        )

        assert result.prepared == {f"{prefix}-audio-profile.bin": expected}
        assert len(expected) == size
        assert result.originals[f"{prefix}-nv425.bin"] == downloaded[425]
        assert result.originals[f"{prefix}-nv426.bin"] == downloaded[426]
        assert result.originals[f"{prefix}-nv440.bin"] == downloaded[440]

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

        assert result.prepared["ta1618-audio-profile.bin"][42:] == (
            b"\x1a\x00\x38\x34\x30\x2c\x28\x24\x20\x1c\x1b"
            b"\x06\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a" + FITTED_NOKIA_PROCESSING_SECTION
        )

    @pytest.mark.parametrize(
        ("offset", "replacement", "change_both", "error"),
        [
            pytest.param(
                3 * 1072 + 466, b"\x1b\x00", False, "Handsfree NV426 differs", id="different-copy"
            ),
            pytest.param(3 * 1072 + 20, b"\x32\x00", True, "speaker-only", id="combined-route"),
            pytest.param(3 * 1072 + 62, b"\x08\x00", True, "expected 9", id="wrong-level-count"),
            pytest.param(3 * 1072 + 68, b"\x01\x00", True, "PA gain", id="analog-gain"),
            pytest.param(
                3 * 1072 + 70, b"\x80\x00", True, "exceeds 127", id="oversized-digital-gain"
            ),
            pytest.param(
                3 * 1072 + 74,
                b"\x39\x00",
                True,
                "not monotonically",
                id="increasing-digital-gain",
            ),
        ],
    )
    def test_speaker_profile_rejects_wrong_route_or_gain_or_disagreeing_copies(
        self, *, offset: int, replacement: bytes, change_both: bool, error: str
    ) -> None:
        """Only matching speaker-only Handsfree calibration can enable its output."""
        downloaded, protected = nokia_audio_records()
        copies = (downloaded, protected) if change_both else (protected,)
        for records in copies:
            arm = bytearray(records[426])
            arm[offset : offset + len(replacement)] = replacement
            records[426] = bytes(arm)
        with pytest.raises(ValueError, match=error):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="ta1618",
                machine_compatible=b"nokia,ta-1618",
            )

    @pytest.mark.parametrize(
        ("replacements", "change_both", "error"),
        [
            pytest.param(
                {1072 + 466: b"\x1b\x00"}, False, "Headfree NV426 differs", id="different-copy"
            ),
            pytest.param(
                {1072: b"Headphone"}, True, "exactly one Headfree mode", id="absent-mode"
            ),
            pytest.param(
                {4 * 1072: b"Headfree"}, True, "exactly one Headfree mode", id="duplicate-mode"
            ),
            pytest.param(
                {1072 + 20: b"\x22\x00"}, True, "Headfree does not select", id="speaker-only-route"
            ),
            pytest.param(
                {1072 + 62: b"\x08\x00"}, True, "Headfree app 0 has 8", id="wrong-level-count"
            ),
            pytest.param(
                {1072 + 74: b"\x4f\x00"}, True, "Headfree digital", id="increasing-digital-gain"
            ),
            pytest.param(
                {1072 + 100: b"\x60\x00"},
                True,
                "analog levels 1..9",
                id="one-analog-level-differs",
            ),
            pytest.param(
                _every_headfree_level(b"\x71\x00"), True, "Headfree PA gain", id="nonzero-pa-gain"
            ),
            pytest.param(
                _every_headfree_level(b"\x10\x00"),
                True,
                "Headfree headphone PGA",
                id="headphone-pga-1",
            ),
            pytest.param(
                _every_headfree_level(b"\x80\x00"),
                True,
                "Headfree headphone PGA",
                id="headphone-pga-8",
            ),
            pytest.param(
                _every_headfree_level(b"\x70\x01"),
                True,
                "Headfree analog level sets",
                id="bits-above-pga",
            ),
        ],
    )
    def test_combined_profile_rejects_wrong_route_or_gain_or_disagreeing_copies(
        self, *, replacements: dict[int, bytes], change_both: bool, error: str
    ) -> None:
        """Only one matching headphone-and-speaker Headfree calibration can drive both outputs."""
        downloaded, protected = nokia_audio_records()
        copies = (downloaded, protected) if change_both else (protected,)
        for records in copies:
            arm = bytearray(records[426])
            for offset, replacement in replacements.items():
                arm[offset : offset + len(replacement)] = replacement
            records[426] = bytes(arm)
        with pytest.raises(ValueError, match=error):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="ta1618",
                machine_compatible=b"nokia,ta-1618",
            )

    @pytest.mark.parametrize(
        ("offset", "replacement", "error"),
        [
            pytest.param(62, b"\x08\x00", "expected 9", id="wrong-level-count"),
            pytest.param(68, b"\x06\x00", "PGA levels 1..9 differ", id="different-pga"),
            pytest.param(70, b"\x80\x00", "exceeds 127", id="oversized-digital-gain"),
            pytest.param(74, b"\x6d\x00", "not monotonically", id="increasing-digital-gain"),
        ],
    )
    def test_profile_rejects_unsafe_headset_values(
        self, *, offset: int, replacement: bytes, error: str
    ) -> None:
        """A fitted profile is emitted only from matching, bounded Headset data."""
        # Both NV426 copies receive each change, so the copies still agree.
        downloaded, protected = inoi_audio_records()
        for records in (downloaded, protected):
            changed = bytearray(records[426])
            changed[offset : offset + len(replacement)] = replacement
            records[426] = bytes(changed)
        with pytest.raises(ValueError, match=error):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="phone",
                machine_compatible=b"vendor,phone",
            )

    def test_profile_rejects_out_of_range_headset_pga(self) -> None:
        """Headset PGA must remain within the supported range even when copies agree."""
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
        with pytest.raises(ValueError, match=r"outside supported range 2..7"):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="phone",
                machine_compatible=b"vendor,phone",
            )

    @pytest.mark.parametrize(
        ("identifier", "offset", "replacement", "error"),
        [
            pytest.param(426, 68, b"\x06\x00", "Headset NV426 differs", id="headset-pga"),
            pytest.param(426, 70, b"\x6b\x00", "Headset NV426 differs", id="headset-digital-gain"),
            pytest.param(440, 20, b"\x01\x00", "EQ_Headset NV440 differs", id="headset-eq"),
            pytest.param(
                440, 3 * 544 + 22, b"\xab\x00", "EQ_Handsfree NV440 differs", id="handsfree-eq"
            ),
        ],
    )
    def test_profile_rejects_nv_copies_that_disagree_on_carried_values(
        self, *, identifier: int, offset: int, replacement: bytes, error: str
    ) -> None:
        """DownloadedNV and ProtectNV must agree on every value carried into the profile."""
        downloaded, protected = inoi_audio_records()
        changed = bytearray(protected[identifier])
        changed[offset : offset + len(replacement)] = replacement
        protected[identifier] = bytes(changed)
        with pytest.raises(ValueError, match=error):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="phone",
                machine_compatible=b"vendor,phone",
            )

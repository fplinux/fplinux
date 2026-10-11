# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic headphone-fed speaker calibration scenarios."""

from __future__ import annotations

import struct

import pytest
from fplinux_cli.device_data.audio_profile import prepare_headset_gain_profile

from tests.small.device_data.audio_expected import FITTED_NOKIA_PROCESSING_SECTION
from tests.small.device_data.audio_fixtures import UNUSED_BAND, eq_set, nokia_audio_records

# Disabled filters pass through at each of the three supported sample rates.
UNITY_SECTIONS = bytes.fromhex("0010 0040 0000 0000 0000 0000") * 18
EXPECTED_PROCESSING = (
    bytes.fromhex("0008 01 e001 0020 fe04 0000 e2ff 1e67 8c02 1000 dd04 0000 6200")
    + UNITY_SECTIONS
    + bytes.fromhex("0010 01 e001 0020 fe04 0000 e2ff 6666 8c02 1000 dd04 0000 6200")
    + UNITY_SECTIONS
    + bytes.fromhex("0018 01 e001 0020 fe04 0000 e2ff 9065 8c02 1000 dd04 0000 6200")
    + UNITY_SECTIONS
)
EXPECTED_PROFILE = (
    b"FPAUDIO\0bq,2830l"
    + b"\0" * 16
    + bytes.fromhex("07 6f 66 5d 54 4b 42 39 30 27")
    + bytes.fromhex("0000 60 58 50 48 40 38 30 28 20")
    + bytes.fromhex("0000 07 50 49 42 3b 34 2d 26 1f 18")
    + EXPECTED_PROCESSING
)


def headphone_fed_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Build synthetic records with separate gains, processing and unused PA words.

    These values describe a controlled format example, rather than measured
    calibration of a phone. Each route uses raw headphone PGA 7 and disabled
    filters; the distinct output scales and ALC ratios identify its processing.
    """
    arm = bytearray(5360)
    modes = (
        (
            0,
            b"Headset",
            0x0400,
            0x0800,
            (
                0x006F0007,
                0x00660007,
                0x005D0007,
                0x00540007,
                0x004B0007,
                0x00420007,
                0x00390007,
                0x00300007,
                0x00270007,
            ),
            0xABCD,
        ),
        (
            3 * 1072,
            b"Handsfree",
            0x1000,
            0x1000,
            (
                0x00600007,
                0x00580007,
                0x00500007,
                0x00480007,
                0x00400007,
                0x00380007,
                0x00300007,
                0x00280007,
                0x00200007,
            ),
            0x1234,
        ),
        (
            1072,
            b"Headfree",
            0x0800,
            0x1800,
            (
                0x00500007,
                0x00490007,
                0x00420007,
                0x003B0007,
                0x00340007,
                0x002D0007,
                0x00260007,
                0x001F0007,
                0x00180007,
            ),
            0xFEDC,
        ),
    )
    for offset, name, control, input_gain, levels, pa_word in modes:
        arm[offset : offset + 16] = name.ljust(16, b"\0")
        struct.pack_into("<H", arm, offset + 20, 0x0012)
        struct.pack_into("<H", arm, offset + 36, 1)
        struct.pack_into("<H", arm, offset + 40, control)
        struct.pack_into("<Hh", arm, offset + 44, 0x000F, input_gain)
        struct.pack_into("<H", arm, offset + 62, 9)
        struct.pack_into("<9I", arm, offset + 68, *levels)
        struct.pack_into("<H", arm, offset + 466, pa_word)
    eq = bytearray(8160)
    for position, name, ratio in (
        (1, b"EQ_Headset", 26398),
        (4, b"EQ_Handsfree", 26214),
        (2, b"EQ_Headfree", 26000),
    ):
        eq[(position - 1) * 544 : position * 544] = eq_set(
            name, band_control=0, bands=(UNUSED_BAND,) * 7, alc_ratio=ratio
        )
    downloaded = {425: b"\x05\x02", 426: bytes(arm), 440: bytes(eq)}
    return downloaded, downloaded.copy()


class HeadphoneFedAudioTests:
    """Protect compact gains, per-route processing and preservation of NV input."""

    def test_separate_route_gains_and_processing_survive_unused_pa_normalization(self) -> None:
        """Keep route-specific bytes while retaining the complete original NV records."""
        downloaded, protected = headphone_fed_records()
        original_downloaded = downloaded.copy()
        original_protected = protected.copy()

        result = prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="bq-2830l-tank-connect-max",
            machine_compatible=b"bq,2830l",
            speaker_from_headphones=True,
        )

        assert result.prepared == {"bq-2830l-tank-connect-max-audio-profile.bin": EXPECTED_PROFILE}
        assert len(EXPECTED_PROFILE) == 788
        assert result.originals == {
            "bq-2830l-tank-connect-max-nv425.bin": original_downloaded[425],
            "bq-2830l-tank-connect-max-nv426.bin": original_downloaded[426],
            "bq-2830l-tank-connect-max-nv440.bin": original_downloaded[440],
        }
        assert downloaded == original_downloaded
        assert protected == original_protected

    def test_enabled_equalizers_keep_their_distinct_route_coefficients(self) -> None:
        """A synthetic headphone-fed topology retains three nontrivial EQ examples."""
        downloaded, protected = nokia_audio_records()
        for records in (downloaded, protected):
            arm = bytearray(records[426])
            for offset in (0, 1072, 3 * 1072):
                struct.pack_into("<H", arm, offset + 20, 0x0012)
                for level in range(9):
                    struct.pack_into("<H", arm, offset + 68 + 4 * level, 7)
            records[426] = bytes(arm)
        original_downloaded = downloaded.copy()
        original_protected = protected.copy()

        result = prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="bq-2830l-tank-connect-max",
            machine_compatible=b"bq,2830l",
            speaker_from_headphones=True,
        )

        assert result.prepared["bq-2830l-tank-connect-max-audio-profile.bin"][32:] == (
            bytes.fromhex("07 47 42 3d 37 32 2c 26 20 1a")
            + bytes.fromhex("0000 38 34 30 2c 28 24 20 1c 1b")
            + bytes.fromhex("0000 07 4e 47 3b 35 2f 2a 24 1f 1a")
            + FITTED_NOKIA_PROCESSING_SECTION
        )
        assert downloaded == original_downloaded
        assert protected == original_protected

    @pytest.mark.parametrize(
        ("mode_offset", "mode_name"),
        [(0, "Headset"), (3 * 1072, "Handsfree"), (1072, "Headfree")],
    )
    @pytest.mark.parametrize("route", [0x0022, 0x0032], ids=["integrated-speaker", "combined"])
    def test_integrated_codec_routes_are_rejected_for_headphone_fed_speakers(
        self, mode_offset: int, mode_name: str, route: int
    ) -> None:
        """Reject integrated-speaker selection in each otherwise admissible mode."""
        downloaded, protected = headphone_fed_records()
        for records in (downloaded, protected):
            arm = bytearray(records[426])
            struct.pack_into("<H", arm, mode_offset + 20, route)
            records[426] = bytes(arm)

        with pytest.raises(ValueError, match=rf"{mode_name}.*headphone-only"):
            prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="bq-2830l-tank-connect-max",
                machine_compatible=b"bq,2830l",
                speaker_from_headphones=True,
            )

    @pytest.mark.parametrize("mode_offset", [3 * 1072, 1072], ids=["Handsfree", "Headfree"])
    @pytest.mark.parametrize(
        ("pga", "all_levels"),
        [(6, True), (6, False), (0x70, True), (1, True), (8, True)],
        ids=[
            "different-fixed-pga",
            "one-step-differs",
            "combined-encoding",
            "too-low",
            "too-high",
        ],
    )
    def test_speaker_modes_require_the_same_raw_pga_as_headset(
        self, mode_offset: int, pga: int, *, all_levels: bool
    ) -> None:
        """A compact speaker profile cannot represent an independent headphone PGA."""
        downloaded, protected = headphone_fed_records()
        for records in (downloaded, protected):
            arm = bytearray(records[426])
            for level in range(9 if all_levels else 1):
                struct.pack_into("<H", arm, mode_offset + 68 + 4 * level, pga)
            records[426] = bytes(arm)

        with pytest.raises(ValueError, match="share the Headset headphone PGA"):
            prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="bq-2830l-tank-connect-max",
                machine_compatible=b"bq,2830l",
                speaker_from_headphones=True,
            )

    @pytest.mark.parametrize(
        ("record_id", "offset", "error"),
        [
            (425, 1, "mode count differs"),
            (426, 20, "Headset NV426 differs"),
            (426, 3 * 1072 + 466, "Handsfree NV426 differs"),
            (426, 1072 + 466, "Headfree NV426 differs"),
            (440, 18, "EQ_Headset NV440 differs"),
            (440, 3 * 544 + 18, "EQ_Handsfree NV440 differs"),
            (440, 544 + 18, "EQ_Headfree NV440 differs"),
        ],
    )
    def test_disagreeing_nv_copies_are_rejected(
        self, record_id: int, offset: int, error: str
    ) -> None:
        """Reject a disagreement in each used mode or selected processing set."""
        downloaded, protected = headphone_fed_records()
        record = bytearray(protected[record_id])
        record[offset] ^= 1
        protected[record_id] = bytes(record)

        with pytest.raises(ValueError, match=error):
            prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="bq-2830l-tank-connect-max",
                machine_compatible=b"bq,2830l",
                speaker_from_headphones=True,
            )

    def test_headphone_fed_routes_are_rejected_by_integrated_speaker_policy(self) -> None:
        """The default topology must keep its speaker-only route admission."""
        downloaded, protected = headphone_fed_records()

        with pytest.raises(ValueError, match=r"Handsfree.*speaker-only"):
            prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="bq-2830l-tank-connect-max",
                machine_compatible=b"bq,2830l",
            )

    def test_headphone_fed_speakers_cannot_also_request_speaker_vibration(self) -> None:
        """Reject incompatible speaker policies before preparing an actuator tone."""
        downloaded, protected = headphone_fed_records()

        with pytest.raises(ValueError, match="vibrate tone"):
            prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="bq-2830l-tank-connect-max",
                machine_compatible=b"bq,2830l",
                speaker_from_headphones=True,
                speaker_vibration=True,
            )

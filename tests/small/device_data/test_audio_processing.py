# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data audio processing scenarios."""

from __future__ import annotations

import struct

import pytest
from fplinux_cli.device_data import audio_profile

from tests.small.device_data.audio_expected import (
    FITTED_NOKIA_PROCESSING_SECTION,
    NOKIA_HANDSFREE_SECTIONS,
    NOKIA_HEADFREE_SECTIONS,
    NOKIA_HEADSET_SECTIONS,
    PASS_THROUGH,
    PROCESSING_OFFSET,
    PROCESSING_ROUTE_SIZE,
    processing_route,
)
from tests.small.device_data.audio_fixtures import inoi_audio_records, nokia_audio_records


class PlaybackProcessingTests:
    """Protect the stock EQ6, output scale and ALC settings designed from fitted EQ sets."""

    @staticmethod
    def _profile(
        records: tuple[dict[int, bytes], dict[int, bytes]], *, speaker_vibration: bool = False
    ) -> bytes:
        """Prepare a profile from DownloadedNV and ProtectNV audio records."""
        downloaded, protected = records
        result = audio_profile.prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="phone",
            machine_compatible=b"vendor,phone",
            speaker_vibration=speaker_vibration,
        )
        return result.prepared["phone-audio-profile.bin"]

    def test_design_tables_reproduce_stock_filter_tables(self) -> None:
        """The generated gain and cosine tables equal entries of the stock firmware tables."""
        gain_entries = {
            0: 1,
            720: 64,
            1080: 515,
            1439: 4072,
            1440: 4096,
            1441: 4119,
            1620: 11544,
            1800: 32535,
        }
        cosine_entries = {0: 16384, 1: 16383, 683: 14186, 1024: 11585, 2047: 12, 2048: 0}

        assert len(audio_profile.LINEAR_GAIN_TABLE) == 1801
        assert len(audio_profile.COSINE_TABLE) == 2049
        for index, value in gain_entries.items():
            assert audio_profile.LINEAR_GAIN_TABLE[index] == value, f"gain table index {index}"
        for index, value in cosine_entries.items():
            assert audio_profile.COSINE_TABLE[index] == value, f"cosine table index {index}"

    def test_fitted_route_words_match_independent_stock_values(self) -> None:
        """S6, ALC and coefficient words sit at their wire offsets with the stock values."""
        profiles = {
            "Nokia": self._profile(nokia_audio_records()),
            "INOI": self._profile(inoi_audio_records(), speaker_vibration=True),
        }
        route_values = (
            ("Nokia", 0, 0x05A6, 26398),
            ("Nokia", 1, 0x0A0C, 26398),
            ("Nokia", 2, 0x071D, 26398),
            ("INOI", 0, 0x071D, 26398),
            ("INOI", 1, 0x0CA6, 26398),
            ("INOI", 2, 0x0400, 26214),
        )
        for phone, route, output_scale, alc_ratio in route_values:
            offset = PROCESSING_OFFSET + route * PROCESSING_ROUTE_SIZE
            assert struct.unpack_from("<HB", profiles[phone], offset) == (
                output_scale,
                1,
            ), f"{phone} route {route} output scale and ALC enable"
            assert struct.unpack_from("<h", profiles[phone], offset + 13)[0] == alc_ratio, (
                f"{phone} route {route} ALC ratio"
            )

        # The stock register image of INOI Handsfree at 48 kHz, sections 0..3,
        # and of the Nokia Headset 100 Hz band at 48 kHz.
        section_values = (
            ("INOI", 1, 0, (4096, 16024, -28232, 28148, 12232, -11953)),
            ("INOI", 1, 1, (4096, 15861, -31712, 31712, 15861, -15338)),
            ("INOI", 1, 2, (4096, 18695, 25902, -25902, 8125, -10437)),
            ("INOI", 1, 3, (4096, 17052, -28507, 28507, 12460, -13128)),
            ("Nokia", 0, 1, (4096, 16345, -32528, 32528, 16185, -16147)),
        )
        for phone, route, section, words in section_values:
            rate_48k = PROCESSING_OFFSET + route * PROCESSING_ROUTE_SIZE + 25 + 2 * 72
            assert struct.unpack_from("<6h", profiles[phone], rate_48k + 12 * section) == words, (
                f"{phone} route {route} section {section}"
            )

    def test_eq_sets_are_selected_by_mode_index_not_by_position(self) -> None:
        """Moving sets in NV440 changes nothing when the modes select their new positions."""
        downloaded, protected = nokia_audio_records()
        for records in (downloaded, protected):
            eq = bytearray(records[440])
            headfree_set = eq[544 : 2 * 544]
            eq[544 : 2 * 544] = eq[3 * 544 : 4 * 544]
            eq[3 * 544 : 4 * 544] = headfree_set
            records[440] = bytes(eq)
            arm = bytearray(records[426])
            struct.pack_into("<H", arm, 1072 + 40, 0x10EC)
            struct.pack_into("<H", arm, 3 * 1072 + 40, 0x0921)
            records[426] = bytes(arm)

        profile = self._profile((downloaded, protected))

        assert profile[PROCESSING_OFFSET:] == FITTED_NOKIA_PROCESSING_SECTION

    def test_shelves_and_other_low_cut_types_pass_through_like_stock(self) -> None:
        """Stock designs no shelf or third low-cut type; those sections pass audio unchanged."""
        downloaded, protected = nokia_audio_records()
        for records in (downloaded, protected):
            eq = bytearray(records[440])
            # Headset marks band 0 as a low shelf and band 4 as a high shelf.
            struct.pack_into("<H", eq, 20, 0xF803)
            # Headfree selects low-cut type 2 instead of the two-shelf design.
            struct.pack_into("<H", eq, 544 + 20, 0xFA80)
            records[440] = bytes(eq)
        headset = tuple(
            (rate[0], PASS_THROUGH, *rate[2:5], PASS_THROUGH) for rate in NOKIA_HEADSET_SECTIONS
        )
        headfree = tuple((PASS_THROUGH, *rate[1:]) for rate in NOKIA_HEADFREE_SECTIONS)

        profile = self._profile((downloaded, protected))

        assert profile[PROCESSING_OFFSET:] == (
            processing_route(0x05A6, 26398, headset)
            + processing_route(0x0A0C, 26398, NOKIA_HANDSFREE_SECTIONS)
            + processing_route(0x071D, 26398, headfree)
        )

    def test_route_alc_enable_and_output_scale_follow_the_selected_set(self) -> None:
        """A set without ALC clears only its route's enable; S6 saturates at 32767."""
        downloaded, protected = nokia_audio_records()
        for records in (downloaded, protected):
            eq = bytearray(records[440])
            struct.pack_into("<H", eq, 3 * 544 + 16, 0x007F)
            struct.pack_into("<h", eq, 544 + 18, 0x2000)
            records[440] = bytes(eq)
            arm = bytearray(records[426])
            struct.pack_into("<h", arm, 1072 + 46, 0x7FFF)
            records[426] = bytes(arm)
        expected = bytearray(FITTED_NOKIA_PROCESSING_SECTION)
        expected[PROCESSING_ROUTE_SIZE + 2] = 0
        expected[2 * PROCESSING_ROUTE_SIZE : 2 * PROCESSING_ROUTE_SIZE + 2] = b"\xff\x7f"

        profile = self._profile((downloaded, protected))

        assert profile[PROCESSING_OFFSET:] == bytes(expected)

    @pytest.mark.parametrize(
        ("identifier", "offset", "value_format", "values", "error"),
        [
            pytest.param(
                426, 3 * 1072 + 44, "<H", (0x001F,), "Handsfree bypasses playback", id="bypass"
            ),
            pytest.param(
                426,
                3 * 1072 + 44,
                "<H",
                (0x0001,),
                "Handsfree equalizer mode is not selected by the player",
                id="fixed-eq-mode",
            ),
            pytest.param(
                426, 3 * 1072 + 40, "<H", (0x0021,), "EQ set 0; expected 1..15", id="set-index-0"
            ),
            pytest.param(
                426, 3 * 1072 + 40, "<H", (0x4021,), "EQ set 16; expected 1..15", id="set-index-16"
            ),
            pytest.param(
                426,
                1072 + 40,
                "<H",
                (0x10EC,),
                "set 4, which is not EQ_Headfree",
                id="other-mode-set",
            ),
            pytest.param(
                426, 3 * 1072 + 46, "<h", (0,), "Handsfree output scale 0 is not", id="zero-s6"
            ),
            pytest.param(
                426,
                3 * 1072 + 46,
                "<h",
                (-1,),
                "Handsfree output scale -1 is not",
                id="negative-s6",
            ),
            pytest.param(
                440, 3 * 544 + 16, "<H", (0x017E,), "output limit is not", id="output-limit"
            ),
            pytest.param(440, 3 * 544 + 16, "<H", (0x817F,), "uses eight bands", id="eight-bands"),
            pytest.param(
                440,
                3 * 544 + 20,
                "<H",
                (0xF980,),
                "EQ_Handsfree Butterworth low-cut filter is not supported",
                id="butterworth-low-cut",
            ),
            pytest.param(
                440, 3 * 544 + 66, "<h", (-200,), "corner frequency is neg", id="negative-corner"
            ),
            pytest.param(
                440,
                3 * 544 + 66,
                "<4h",
                (0, 0, 0, 0),
                "EQ_Handsfree section 0 is unstable at 24000 Hz",
                id="zero-low-cut-corners",
            ),
            pytest.param(
                440,
                3 * 544 + 22,
                "<8h",
                (1000, 512, 50, 0, 1000, 512, 50, 0),
                r"EQ_Handsfree boosts 24000 Hz playback by [0-9.]+ dB, above the 6 dB",
                id="two-5-db-bands-at-1-khz",
            ),
            # A dense scan of the running response puts the peak of this
            # narrowest possible +7 dB band at 6.41 dB.
            pytest.param(
                440,
                3 * 544 + 38,
                "<4h",
                (2500, 32767, 70, 0),
                "EQ_Handsfree boosts 24000 Hz playback by 6.4 dB, above the 6 dB",
                id="narrow-7-db-band-at-2_5-khz",
            ),
        ],
    )
    def test_profile_rejects_unsupported_or_unsafe_processing(
        self,
        *,
        identifier: int,
        offset: int,
        value_format: str,
        values: tuple[int, ...],
        error: str,
    ) -> None:
        """Only a supported, stable EQ set within the datapath headroom produces a profile."""
        downloaded, protected = nokia_audio_records()
        for records in (downloaded, protected):
            changed = bytearray(records[identifier])
            struct.pack_into(value_format, changed, offset, *values)
            records[identifier] = bytes(changed)
        with pytest.raises(ValueError, match=error):
            self._profile((downloaded, protected))

# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic headphone-only Headfree playback profile behavior."""

from __future__ import annotations

import struct

import pytest
from fplinux_cli.device_data.audio_profile import prepare_headset_gain_profile

from tests.small.device_data.headfree_fixtures import headphone_only_headfree_records

UNITY_SECTIONS = bytes.fromhex("0010 0040 0000 0000 0000 0000") * 18
EXPECTED_PROCESSING = (
    bytes.fromhex("0008 01 e001 0020 fe04 0000 e2ff 1e67 8c02 1000 dd04 0000 6200")
    + UNITY_SECTIONS
    + bytes.fromhex("0010 01 e001 0020 fe04 0000 e2ff 6666 8c02 1000 dd04 0000 6200")
    + UNITY_SECTIONS
    + bytes.fromhex("0018 01 e001 0020 fe04 0000 e2ff 9065 8c02 1000 dd04 0000 6200")
    + UNITY_SECTIONS
)


def test_headfree_raw_pga_gains_and_processing_remain_distinct_from_headset() -> None:
    """The explicit policy preserves admitted values without synthesizing a speaker route."""
    downloaded, protected = headphone_only_headfree_records()
    original_downloaded = downloaded.copy()
    original_protected = protected.copy()

    result = prepare_headset_gain_profile(
        downloaded,
        protected,
        prefix="example",
        machine_compatible=b"example,phone",
        headfree_headphones_only=True,
    )

    expected = (
        b"FPAUDIO\0example,phone"
        + b"\0" * 11
        + bytes.fromhex("04 5f 57 4f 47 3f 37 2f 27 1f")
        + bytes.fromhex("2b00 57 4f 47 3f 37 2f 27 1f 17")
        + bytes.fromhex("1a00 06 5a 52 4a 42 3a 32 2a 22 1a")
        + EXPECTED_PROCESSING
    )
    assert result.prepared == {"example-audio-profile.bin": expected}
    assert result.originals == {
        "example-nv425.bin": original_downloaded[425],
        "example-nv426.bin": original_downloaded[426],
        "example-nv440.bin": original_downloaded[440],
    }
    assert downloaded == original_downloaded
    assert protected == original_protected
    assert struct.unpack_from("<H", result.originals["example-nv426.bin"], 1092)[0] == 0x12


@pytest.mark.parametrize("route", [0x22, 0x32], ids=["speaker", "combined"])
def test_headphone_only_headfree_policy_rejects_another_codec_route(route: int) -> None:
    """A target policy cannot reinterpret speaker-bearing fixed records as headphones."""
    downloaded, protected = headphone_only_headfree_records()
    for records in (downloaded, protected):
        arm = bytearray(records[426])
        struct.pack_into("<H", arm, 1092, route)
        records[426] = bytes(arm)

    with pytest.raises(ValueError, match=r"Headfree.*headphone-only"):
        prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="example",
            machine_compatible=b"example,phone",
            headfree_headphones_only=True,
        )


def test_headfree_raw_pga_is_not_decoded_as_a_combined_gain_word() -> None:
    """Reject a combined-route encoding under headphone-only policy."""
    downloaded, protected = headphone_only_headfree_records()
    for records in (downloaded, protected):
        arm = bytearray(records[426])
        for index in range(9):
            struct.pack_into("<H", arm, 1140 + index * 4, 0x60)
        records[426] = bytes(arm)

    with pytest.raises(ValueError, match=r"Headfree headphone PGA.*supported range"):
        prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="example",
            machine_compatible=b"example,phone",
            headfree_headphones_only=True,
        )


def test_headphone_only_headfree_still_requires_the_two_fixed_modes_to_agree() -> None:
    """Reject disagreement in a field carried into the fitted playback profile."""
    downloaded, protected = headphone_only_headfree_records()
    arm = bytearray(protected[426])
    arm[1142] ^= 1
    protected[426] = bytes(arm)

    with pytest.raises(ValueError, match="Headfree NV426 differs"):
        prepare_headset_gain_profile(
            downloaded,
            protected,
            prefix="example",
            machine_compatible=b"example,phone",
            headfree_headphones_only=True,
        )

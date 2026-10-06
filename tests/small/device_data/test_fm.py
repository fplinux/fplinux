# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data fm scenarios."""

from __future__ import annotations

import pytest
from fplinux_cli.device_data import fm_radio
from fplinux_cli.device_data import formats as device_data

from tests.small.device_data.nv_fixtures import nv1


class FmRadioConfigTests:
    """Protect the fitted FM payload consumed by the radio ENABLE operation."""

    def test_matching_nv419_becomes_the_exact_normalized_fm_payload(self) -> None:
        """ENABLE zeros the first word and duplicates th1 while preserving other bytes."""
        original = bytes(range(128))
        stream = nv1(((419, original),))
        downloaded = device_data.fixed_nv_records(stream, {419: 128})
        protected = device_data.fixed_nv_records(stream, {419: 128})

        result = fm_radio.prepare_fm_config(downloaded, protected, prefix="phone")

        expected = bytes.fromhex(
            "00 00 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f "
            "0e 0f 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f "
            "20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f "
            "30 31 32 33 34 35 36 37 38 39 3a 3b 3c 3d 3e 3f "
            "40 41 42 43 44 45 46 47 48 49 4a 4b 4c 4d 4e 4f "
            "50 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f "
            "60 61 62 63 64 65 66 67 68 69 6a 6b 6c 6d 6e 6f "
            "70 71 72 73 74 75 76 77 78 79 7a 7b 7c 7d 7e 7f"
        )
        assert (result.originals) == ({"phone-nv419.bin": original})
        assert (result.prepared) == ({"phone-fm-config.bin": expected})
        assert (len(expected)) == (128)

    def test_conflicting_nv419_copies_cannot_produce_a_payload(self) -> None:
        """Disagreeing fixed NV419 copies cannot become fitted FM input."""
        original = bytes(range(128))
        different = bytearray(original)
        different[64] ^= 1
        with pytest.raises(ValueError, match="NV419 differs"):
            fm_radio.prepare_fm_config(
                {419: original},
                {419: bytes(different)},
                prefix="phone",
            )

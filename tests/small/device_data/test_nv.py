# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data nv scenarios."""

from __future__ import annotations

import struct
import unittest

from fplinux_cli.device_data import bluetooth_firmware as bluetooth
from fplinux_cli.device_data import formats as device_data

from tests.small.device_data.nv_fixtures import FIXED_RECORD_SIZES, fixed_records, nv1, running_nv


class FixedNvFormatTests(unittest.TestCase):
    """Protect fitted-record selection from complete sorted fixed-NV streams."""

    def test_fixed_stream_uses_record_ids_and_lengths_not_fixed_offsets(self) -> None:
        """Unrelated record size and generation changes preserve selected values."""
        expected = fixed_records()
        for unrelated in (b"odd", b"a longer unrelated value"):
            for generation in (1, 0x10203040):
                with self.subTest(unrelated=unrelated, generation=generation):
                    stream = nv1(((2, unrelated), *expected.items()), generation)
                    self.assertEqual(
                        device_data.fixed_nv_records(stream, FIXED_RECORD_SIZES),
                        expected,
                    )

    def test_fixed_stream_rejects_incomplete_or_ambiguous_records(self) -> None:
        """Incomplete or invalid record streams do not yield partial calibration."""
        complete = nv1(tuple(fixed_records().items()))
        cases = {
            "missing RF record": (
                nv1(((401, b"A" * 8), (402, b"B" * 176))),
                "fixed NV record 404 is missing or has the wrong size",
            ),
            "wrong address record length": (
                nv1(((401, b"A" * 7), (402, b"B" * 176), (404, b"C" * 252))),
                "fixed NV record 401 is missing or has the wrong size",
            ),
            "duplicate identifier": (
                nv1(
                    (
                        (401, b"A" * 8),
                        (401, b"A" * 8),
                        (402, b"B" * 176),
                        (404, b"C" * 252),
                    )
                ),
                "damaged fixed NV record ordering",
            ),
            "out-of-order identifier": (
                nv1(((402, b"B" * 176), (401, b"A" * 8), (404, b"C" * 252))),
                "damaged fixed NV record ordering",
            ),
            "truncated payload": (
                complete[:-20],
                "truncated fixed NV record",
            ),
            "missing terminator": (
                complete[:-4],
                "fixed NV stream has no complete terminator",
            ),
        }
        for name, (stream, error) in cases.items():
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                device_data.fixed_nv_records(stream, FIXED_RECORD_SIZES)


class BluetoothRunningNvFormatTests(unittest.TestCase):
    """Protect refusal to guess among conflicting Bluetooth values."""

    def test_running_records_accept_redundant_agreeing_copies(self) -> None:
        """Consistent historical copies corroborate the same fitted settings."""
        running = running_nv()
        # These bytes resemble an ID/length but have no valid wrapper header.
        unrelated = b"\x00" * 4 + b"\x91\x01\x08\x00" + b"\x00" * 24
        bluetooth.verify_running_nv(running + unrelated + running, fixed_records())

    def test_running_records_reject_damaged_missing_or_conflicting_values(self) -> None:
        """Do not substitute unverified data or guess which physical copy is newest."""
        running = running_nv()
        damaged = bytearray(running)
        damaged[128 + 12] ^= 1
        different_valid_address = struct.pack("<HHHHI", 0xFE65, 0xF6F6, 401, 8, 1) + b"B" * 8
        cases = {
            "damaged payload checksum": (bytes(damaged), "damaged RunningNV checksum"),
            "missing records": (b"\xff" * 256, "no checksum-valid RunningNV copy"),
            "conflicting historical copy": (running + different_valid_address, "ambiguous"),
            "truncated last record": (running[:-1], "no checksum-valid RunningNV copy"),
        }
        for name, (data, error) in cases.items():
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                bluetooth.verify_running_nv(data, fixed_records())

    def test_checksum_handles_odd_bytes_and_end_around_carry(self) -> None:
        """Literal format vectors cover padding-independent checksum behavior."""
        for payload, expected in (
            (b"", 0xFFFF),
            (b"\x01\x02\x03", 0xFDFB),
            (b"\xff\xff\x01\x00", 0xFFFE),
        ):
            with self.subTest(payload=payload):
                self.assertEqual(bluetooth.nv_checksum(payload), expected)


if __name__ == "__main__":
    unittest.main()

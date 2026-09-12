# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic binary-format checks; no vendor image or physical NAND fixture."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli import bluetooth_firmware as firmware

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]


def _load_firmware(target: str, filename: str) -> ModuleType:
    path = ROOT / "targets" / target / filename
    spec = importlib.util.spec_from_file_location(
        f"{target.replace('-', '_')}_firmware_formats", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load target firmware module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _nv1(records: tuple[tuple[int, bytes], ...], generation: int = 0x12345678) -> bytes:
    """Serialize synthetic records according to the NV1 wire format."""
    parts = [struct.pack("<I", generation)]
    for identifier, payload in records:
        parts.extend(
            (
                struct.pack("<HH", identifier, len(payload)),
                payload,
                bytes(-len(payload) % 4),
            )
        )
    return b"".join(parts) + b"\xff\xff\xff\xff"


def _fixed_records() -> dict[int, bytes]:
    """Use visibly synthetic values, independent of production's record registry."""
    return {401: b"A" * 8, 402: b"B" * 176, 404: b"C" * 252}


def _running_nv() -> bytes:
    """Use literal checksums, independently calculated for the repeated-byte data.

    For example, eight 'A' bytes sum to 4 * 0x4141; the folded complement is
    0xfafa. No expected checksum is obtained from the production checksum code.
    """
    return b"".join(
        (
            b"\xff" * 128,
            struct.pack("<HHHHI", 0xFE65, 0xFAFA, 401, 8, 1) + b"A" * 8,
            struct.pack("<HHHHI", 0xFDBC, 0x3939, 402, 176, 1) + b"B" * 176,
            struct.pack("<HHHHI", 0xFD6E, 0xE4E4, 404, 252, 1) + b"C" * 252,
        )
    )


class PhysicalPageFormatTests(unittest.TestCase):
    """Keep the physical main/OOB format separate from image compatibility."""

    def test_main_read_crosses_pages_without_copying_oob(self) -> None:
        """Both supported geometries exclude spare bytes from cross-page reads."""
        for spare in (64, 128):
            with self.subTest(spare=spare):
                raw = b"A" * 2048 + b"O" * spare + b"B" * 2048 + b"P" * spare
                nand = firmware.PhysicalNand(raw, 2048 + spare)

                self.assertEqual(nand.main_bytes(2044, 12), b"AAAA" + b"B" * 8)
                self.assertEqual(nand.main_bytes(0, 4096), b"A" * 2048 + b"B" * 2048)
                with self.assertRaisesRegex(ValueError, "outside"):
                    nand.main_bytes(4090, 7)

    def test_either_factory_marker_rejects_a_selected_block(self) -> None:
        """Neither factory marker may be ignored for a consumed block."""
        for page_bytes in (2112, 2176):
            unmarked = b"\xff" * (64 * page_bytes)
            firmware.PhysicalNand(unmarked, page_bytes).require_good_blocks(0, 2048)
            for marker in (2048, page_bytes + 2048):
                with self.subTest(page_bytes=page_bytes, marker=marker):
                    marked = bytearray(unmarked)
                    marked[marker] = 0
                    with self.assertRaisesRegex(ValueError, "block 0 is marked bad"):
                        firmware.PhysicalNand(bytes(marked), page_bytes).require_good_blocks(
                            0, 2048
                        )

    def test_each_target_rejects_incomplete_input_before_extraction(self) -> None:
        """A fragment cannot be presented as a completed physical backup."""
        for target, filename, expected_size in (
            ("nokia-ta1618", "bluetooth_firmware.py", 142606336),
            ("inoi-240-modern-4g", "inoi240_bluetooth_firmware.py", 138412032),
            ("inoi-244-modern-4g", "inoi244_bluetooth_firmware.py", 138412032),
        ):
            parser = _load_firmware(target, filename)
            for raw in (b"", b"\xff" * 2176, b"\xff" * 2112):
                with (
                    self.subTest(target=target, size=len(raw)),
                    self.assertRaisesRegex(
                        ValueError, f"complete {expected_size}-byte physical NAND backup"
                    ),
                ):
                    parser.prepare_firmware(raw)


class BluetoothNvFormatTests(unittest.TestCase):
    """Protect fitted-record selection and refusal to guess among conflicting values."""

    def test_fixed_stream_uses_record_ids_and_lengths_not_fixed_offsets(self) -> None:
        """Unrelated record size and generation changes preserve Bluetooth values."""
        expected = _fixed_records()
        for unrelated in (b"odd", b"a longer unrelated value"):
            for generation in (1, 0x10203040):
                with self.subTest(unrelated=unrelated, generation=generation):
                    stream = _nv1(((2, unrelated), *expected.items()), generation)
                    self.assertEqual(firmware.fixed_nv(stream), expected)

    def test_fixed_stream_rejects_incomplete_or_ambiguous_records(self) -> None:
        """Incomplete or invalid record streams do not yield partial calibration."""
        complete = _nv1(tuple(_fixed_records().items()))
        cases = {
            "missing RF record": _nv1(((401, b"A" * 8), (402, b"B" * 176))),
            "wrong address record length": _nv1(
                ((401, b"A" * 7), (402, b"B" * 176), (404, b"C" * 252))
            ),
            "duplicate identifier": _nv1(((401, b"A" * 8), (401, b"D" * 8))),
            "out-of-order identifier": _nv1(((402, b"B" * 176), (401, b"A" * 8))),
            "truncated payload": complete[:-20],
            "missing terminator": complete[:-4],
        }
        for name, stream in cases.items():
            with self.subTest(name=name), self.assertRaises(ValueError):
                firmware.fixed_nv(stream)

    def test_running_records_accept_redundant_agreeing_copies(self) -> None:
        """Consistent historical copies corroborate the same fitted settings."""
        running = _running_nv()
        # These bytes resemble an ID/length but have no valid wrapper header.
        unrelated = b"\x00" * 4 + b"\x91\x01\x08\x00" + b"\x00" * 24
        firmware.verify_running_nv(running + unrelated + running, _fixed_records())

    def test_running_records_reject_damaged_missing_or_conflicting_values(self) -> None:
        """Do not substitute unverified data or guess which physical copy is newest."""
        running = _running_nv()
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
                firmware.verify_running_nv(data, _fixed_records())

    def test_checksum_handles_odd_bytes_and_end_around_carry(self) -> None:
        """Literal format vectors cover padding-independent checksum behavior."""
        for payload, expected in (
            (b"", 0xFFFF),
            (b"\x01\x02\x03", 0xFDFB),
            (b"\xff\xff\x01\x00", 0xFFFE),
        ):
            with self.subTest(payload=payload):
                self.assertEqual(firmware.nv_checksum(payload), expected)


class Cm4CompatibilityOperationTests(unittest.TestCase):
    """Check exact image admission and copy semantics with synthetic instructions."""

    @staticmethod
    def _images() -> tuple[bytes, bytes]:
        original = b"prefix" + b"\x2d\x4c" + b"middle" + b"\xe0\x6d" + b"suffix"
        expected = b"prefix" + b"\x08\xe0" + b"middle" + b"\x2b\xe0" + b"suffix"
        return original, expected

    def test_instruction_rewrite_changes_only_the_output_copy(self) -> None:
        """The synthetic body retains every byte except the two branch instructions."""
        original, expected = self._images()
        before = bytes(original)

        self.assertEqual(firmware.omit_initial_pub_policy(original, (6, 14)), expected)
        self.assertEqual(original, before)

    def test_revision_admits_only_the_exact_original_and_prepared_images(self) -> None:
        """Wrong source identity or output identity cannot produce an admitted image."""
        original, expected = self._images()
        revision = firmware.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256=hashlib.sha256(expected).hexdigest(),
            pub_policy_offsets=(6, 14),
        )

        self.assertEqual(revision.prepare(original), expected)
        for wrong in (original[:-1], original[:-1] + b"!"):
            with self.subTest(wrong=wrong), self.assertRaisesRegex(ValueError, "original CM4"):
                revision.prepare(wrong)
        wrong_output = firmware.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256="0" * 64,
            pub_policy_offsets=(6, 14),
        )
        with self.assertRaisesRegex(ValueError, "unexpected image"):
            wrong_output.prepare(original)
        with self.assertRaisesRegex(ValueError, "patch instructions"):
            firmware.omit_initial_pub_policy(bytes(22), (6, 14))


class PartitionPreparationTests(unittest.TestCase):
    """Exercise extraction and copy preservation on synthetic physical pages."""

    @staticmethod
    def _inputs(
        protected_address: bytes = b"A" * 8,
    ) -> tuple[firmware.PhysicalNand, dict[int, tuple[int, int]], firmware.Cm4Revision]:
        original = b"prefix\x2d\x4cmiddle\xe0\x6dsuffix"
        prepared = b"prefix\x08\xe0middle\x2b\xe0suffix"
        payloads = (
            original,
            _nv1(tuple(_fixed_records().items())),
            _nv1(((401, protected_address), (402, b"B" * 176), (404, b"C" * 252))),
            _running_nv(),
        )
        main = b"".join(payload.ljust(131072, b"\xff") for payload in payloads)
        raw = b"".join(
            main[offset : offset + 2048] + b"\xff" * 64 for offset in range(0, len(main), 2048)
        )
        partitions = {
            0x10000018: (0, 131072),
            0x10000001: (131072, 131072),
            0x1000000F: (262144, 131072),
            0x10000003: (393216, 131072),
        }
        revision = firmware.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256=hashlib.sha256(prepared).hexdigest(),
            pub_policy_offsets=(6, 14),
        )
        return firmware.PhysicalNand(raw, 2112), partitions, revision

    def test_complete_set_keeps_original_image_and_individual_nv_bytes(self) -> None:
        """Only the prepared CM4 changes; every original remains byte-exact."""
        nand, partitions, revision = self._inputs()
        backup_before = bytes(nand.raw)
        expected_originals = {
            "example-cm4.bin": b"prefix\x2d\x4cmiddle\xe0\x6dsuffix",
            "example-bt-config.bin": b"A" * 8,
            "example-bt-sprd.bin": b"B" * 176,
            "example-bt-rf-config.bin": b"C" * 252,
        }

        result = firmware.prepare_from_partitions(
            nand, partitions, prefix="example", revision=revision
        )

        self.assertEqual(result.originals, expected_originals)
        self.assertEqual(
            result.prepared,
            expected_originals | {"example-cm4.bin": b"prefix\x08\xe0middle\x2b\xe0suffix"},
        )
        self.assertEqual(nand.raw, backup_before)

    def test_conflicting_protected_nv_cannot_produce_a_firmware_set(self) -> None:
        """Disagreeing fixed copies are rejected instead of selecting one address."""
        nand, partitions, revision = self._inputs(protected_address=b"D" * 8)

        with self.assertRaisesRegex(ValueError, "DownloadedNV and ProtectNV disagree"):
            firmware.prepare_from_partitions(nand, partitions, prefix="example", revision=revision)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data format checks; no vendor image or physical NAND fixture."""

from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

from fplinux_cli import audio_profile, device_data
from fplinux_cli import bluetooth_firmware as bluetooth

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
FIXED_RECORD_SIZES = {401: 8, 402: 176, 404: 252}
VBM_COPY_OFFSETS = (0x20000, 0x40000)
NOKIA_PARTI_OFFSET = 0x63DE0


class FakeNandPartitionReader:
    """Replace a full physical dump with exact small main-area table fragments."""

    def __init__(self, fragments: dict[int, bytes], *, block_count: int = 1024) -> None:
        """Retain only the fragments and capacity exposed to the parser."""
        self._fragments = fragments
        self._block_count = block_count

    @property
    def block_count(self) -> int:
        """Return the declared fake physical capacity."""
        return self._block_count

    def partition_bytes(self, extent: tuple[int, int]) -> bytes:
        """Return the requested bytes from one declared main-area fragment."""
        offset, size = extent
        for fragment_offset, fragment in self._fragments.items():
            relative = offset - fragment_offset
            if 0 <= relative < len(fragment):
                return fragment[relative : relative + size]
        raise AssertionError(f"unexpected fake NAND read at {offset:#x} for {size} bytes")


def _required_partitions() -> dict[int, device_data.RequiredPartition]:
    """Name the four consumers and their literal fitted-table attributes."""
    return {
        0x10000001: device_data.RequiredPartition("DownloadedNV", 0x100),
        0x1000000F: device_data.RequiredPartition("ProtectNV", 0x100),
        0x10000018: device_data.RequiredPartition("CM4", 0x100),
        0x10000003: device_data.RequiredPartition("RunningNV", 0x001),
    }


def _vbm_table(
    entries: tuple[tuple[int, int, int, int], ...],
    *,
    peer_fields: bytes = bytes(10),
    declared_count: int | None = None,
) -> bytes:
    """Encode the documented VBM header and packed inclusive-block records."""
    if len(peer_fields) != 10:
        message = "peer_fields must cover offsets 0xc..0x15"
        raise ValueError(message)
    count = len(entries) if declared_count is None else declared_count
    return b"".join(
        (
            b"VBM_BOOT",
            struct.pack("<I", 0x102),
            peer_fields,
            struct.pack("<H", count),
            *(struct.pack("<IHHH", *entry) for entry in entries),
        )
    )


def _vbm_entries() -> tuple[tuple[int, int, int, int], ...]:
    """Use literal non-overlapping records, including all four data consumers."""
    return (
        (0x00000001, 0x100, 0, 1),
        (0x10000001, 0x100, 4, 11),
        (0x1000000F, 0x100, 13, 20),
        (0x10000018, 0x100, 116, 123),
        (0x10000003, 0x001, 157, 204),
        (0x00000008, 0x001, 701, 980),
    )


def _nokia_parti_entries() -> tuple[tuple[int, int, int, int], ...]:
    """Use the fitted 22-record PartI shape as an explicit structural fixture."""
    return (
        (0x00000001, 0x100, 0, 2),
        (0x00000002, 0x100, 2, 2),
        (0x10000001, 0x100, 4, 8),
        (0x10000002, 0x100, 12, 1),
        (0x1000000F, 0x100, 13, 8),
        (0x10000006, 0x100, 21, 25),
        (0x10000005, 0x100, 46, 62),
        (0x10000013, 0x100, 108, 8),
        (0x10000018, 0x100, 116, 8),
        (0x10000019, 0x100, 124, 1),
        (0x1000001A, 0x001, 125, 32),
        (0x10000003, 0x001, 157, 48),
        (0x00000003, 0x100, 205, 112),
        (0x00000004, 0x100, 317, 128),
        (0x10000004, 0x100, 445, 192),
        (0x10000007, 0x001, 637, 64),
        (0x1000001B, 0x101, 701, 6),
        (0x1000001C, 0x101, 707, 2),
        (0x1000001D, 0x101, 709, 1),
        (0x10000022, 0x101, 710, 10),
        (0x10000020, 0x101, 720, 160),
        (0x00000008, 0x001, 880, 0xFFFFFFFF),
    )


def _nokia_parti_table(
    entries: tuple[tuple[int, int, int, int], ...],
    *,
    declared_count: int | None = None,
) -> bytes:
    """Encode the compiled PartI count and fixed-width records."""
    count = len(entries) if declared_count is None else declared_count
    return struct.pack("<I", count) + b"".join(struct.pack("<4I", *entry) for entry in entries)


def _load_target_parser(target: str, filename: str) -> ModuleType:
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


def _headset_audio_records(
    levels: tuple[int, ...],
    *,
    pass_band_control: int,
) -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Build matching fitted Headset records from explicit packed NV level values."""
    if len(levels) != 9:
        message = "the synthetic Headset fixture requires nine levels"
        raise ValueError(message)
    arm = bytearray(5360)
    arm[:16] = b"Headset".ljust(16, b"\0")
    struct.pack_into("<H", arm, 36, 1)
    struct.pack_into("<H", arm, 62, 9)
    struct.pack_into("<9I", arm, 68, *levels)
    eq = bytearray(8160)
    eq[:16] = b"EQ_Headset".ljust(16, b"\0")
    struct.pack_into("<H", eq, 20, pass_band_control)
    downloaded = {425: b"\x05\x02", 426: bytes(arm), 440: bytes(eq)}
    protected_arm = bytearray(arm)
    protected_arm[2 * 1072] = 1
    protected_eq = bytearray(eq)
    protected_eq[11 * 544] = 1
    protected = {425: b"\x05\x02", 426: bytes(protected_arm), 440: bytes(protected_eq)}
    return downloaded, protected


def _inoi_audio_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Use the literal packed values observed in both fitted INOI phones."""
    return _headset_audio_records(
        (
            0x006C0007,
            0x00610007,
            0x00560007,
            0x004B0007,
            0x00410007,
            0x00370007,
            0x002D0007,
            0x00230007,
            0x001B0007,
        ),
        pass_band_control=0,
    )


def _nokia_audio_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Use the literal packed values observed in the fitted Nokia TA-1618."""
    return _headset_audio_records(
        (
            0x00470006,
            0x00420006,
            0x003D0006,
            0x00370006,
            0x00320006,
            0x002C0006,
            0x00260006,
            0x00200006,
            0x001A0006,
        ),
        pass_band_control=1,
    )


class PhysicalPageFormatTests(unittest.TestCase):
    """Keep the physical main/OOB format separate from image compatibility."""

    def test_main_read_crosses_pages_without_copying_oob(self) -> None:
        """Both supported geometries exclude spare bytes from cross-page reads."""
        for spare in (64, 128):
            with self.subTest(spare=spare):
                raw = b"A" * 2048 + b"O" * spare + b"B" * 2048 + b"P" * spare
                nand = device_data.PhysicalNand(raw, 2048 + spare)

                self.assertEqual(nand.main_bytes(2044, 12), b"AAAA" + b"B" * 8)
                self.assertEqual(nand.main_bytes(0, 4096), b"A" * 2048 + b"B" * 2048)
                with self.assertRaisesRegex(ValueError, "outside"):
                    nand.main_bytes(4090, 7)

    def test_either_factory_marker_rejects_a_selected_block(self) -> None:
        """Neither factory marker may be ignored for a consumed block."""
        for page_bytes in (2112, 2176):
            unmarked = b"\xff" * (64 * page_bytes)
            device_data.PhysicalNand(unmarked, page_bytes).require_good_blocks(0, 2048)
            for marker in (2048, page_bytes + 2048):
                with self.subTest(page_bytes=page_bytes, marker=marker):
                    marked = bytearray(unmarked)
                    marked[marker] = 0
                    with self.assertRaisesRegex(ValueError, "block 0 is marked bad"):
                        device_data.PhysicalNand(bytes(marked), page_bytes).require_good_blocks(
                            0, 2048
                        )

    def test_complete_dump_size_is_validated_by_physical_geometry(self) -> None:
        """A fragment is rejected before any target partition policy interprets it."""
        for page_bytes, expected_size in ((2112, 138412032), (2176, 142606336)):
            for raw in (b"", b"\xff" * 2176, b"\xff" * 2112):
                with (
                    self.subTest(page_bytes=page_bytes, size=len(raw)),
                    self.assertRaisesRegex(
                        ValueError, f"complete {expected_size}-byte physical NAND backup"
                    ),
                ):
                    device_data.PhysicalNand.from_dump(raw, page_bytes=page_bytes)


class VbmPartitionFormatTests(unittest.TestCase):
    """Protect structural VBM admission without freezing the whole fitted table."""

    @staticmethod
    def _reader(
        first_entries: tuple[tuple[int, int, int, int], ...],
        second_entries: tuple[tuple[int, int, int, int], ...] | None = None,
    ) -> FakeNandPartitionReader:
        if second_entries is None:
            second_entries = first_entries
        return FakeNandPartitionReader(
            {
                VBM_COPY_OFFSETS[0]: _vbm_table(
                    first_entries,
                    peer_fields=b"first-copy",
                ),
                VBM_COPY_OFFSETS[1]: _vbm_table(
                    second_entries,
                    peer_fields=b"secondcopy",
                ),
            }
        )

    def test_matching_copies_admit_only_named_partitions_and_ignore_peer_fields(self) -> None:
        """Different copy metadata does not hide equal decoded consumer descriptors."""
        partitions = device_data.redundant_vbm_partitions(
            self._reader(_vbm_entries()),
            VBM_COPY_OFFSETS,
            _required_partitions(),
        )

        self.assertEqual(
            partitions,
            {
                0x10000001: (0x00080000, 0x00100000),
                0x1000000F: (0x001A0000, 0x00100000),
                0x10000018: (0x00E80000, 0x00100000),
                0x10000003: (0x013A0000, 0x00600000),
            },
        )

    def test_agreed_safe_table_changes_are_not_rejected_as_unknown_images(self) -> None:
        """An unrelated attribute and a required location may change when both copies agree."""
        entries = list(_vbm_entries())
        entries[3] = (0x10000018, 0x100, 124, 131)
        entries[-1] = (0x00000099, 0xBEEF, 701, 980)

        partitions = device_data.redundant_vbm_partitions(
            self._reader(tuple(entries)),
            VBM_COPY_OFFSETS,
            _required_partitions(),
        )

        self.assertEqual(partitions[0x10000018], (0x00F80000, 0x00100000))
        self.assertNotIn(0x00000099, partitions)

    def test_copies_must_have_identical_decoded_counts_and_entries(self) -> None:
        """One changed record is ambiguous even when both copies remain individually safe."""
        second_entries = list(_vbm_entries())
        second_entries[-1] = (0x00000008, 0x0101, 701, 980)

        with self.assertRaisesRegex(ValueError, "redundant VBM.*disagree"):
            device_data.redundant_vbm_partitions(
                self._reader(_vbm_entries(), tuple(second_entries)),
                VBM_COPY_OFFSETS,
                _required_partitions(),
            )

    def test_structural_or_required_partition_damage_is_rejected_precisely(self) -> None:
        """Malformed extents and missing consumer semantics cannot reach extraction."""
        base = list(_vbm_entries())
        cases: dict[str, tuple[tuple[tuple[int, int, int, int], ...], str]] = {}

        duplicate = [*base, (0x00000001, 0x100, 300, 300)]
        cases["duplicate ID"] = (tuple(duplicate), "duplicate VBM partition ID")

        missing = list(base)
        missing[4] = (0x10000099, 0x001, 157, 204)
        cases["missing required ID"] = (tuple(missing), "RunningNV.*is missing")

        overlap = list(base)
        overlap[-1] = (0x00000008, 0x001, 200, 300)
        cases["overlapping extents"] = (tuple(overlap), "VBM partitions .* overlap")

        reversed_extent = list(base)
        reversed_extent[-1] = (0x00000008, 0x001, 300, 299)
        cases["zero or reversed extent"] = (
            tuple(reversed_extent),
            "zero or reversed extent",
        )

        outside = list(base)
        outside[-1] = (0x00000008, 0x001, 1000, 1024)
        cases["outside NAND"] = (tuple(outside), "outside the 1024-block NAND")

        wrong_attributes = list(base)
        wrong_attributes[3] = (0x10000018, 0x001, 116, 123)
        cases["wrong required attributes"] = (
            tuple(wrong_attributes),
            "CM4 has attributes 0x1; expected 0x100",
        )

        for name, (entries, error) in cases.items():
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                device_data.redundant_vbm_partitions(
                    self._reader(entries),
                    VBM_COPY_OFFSETS,
                    _required_partitions(),
                )

    def test_declared_count_is_bounded_and_complete_records_are_required(self) -> None:
        """The count cannot trigger an unbounded read or admit a truncated final record."""
        for count in (0, 101):
            table = _vbm_table((), declared_count=count)
            reader = FakeNandPartitionReader(dict.fromkeys(VBM_COPY_OFFSETS, table))
            with (
                self.subTest(count=count),
                self.assertRaisesRegex(ValueError, f"partition count {count}.*bound"),
            ):
                device_data.redundant_vbm_partitions(
                    reader,
                    VBM_COPY_OFFSETS,
                    _required_partitions(),
                )

        truncated = _vbm_table(_vbm_entries())[:-1]
        reader = FakeNandPartitionReader(dict.fromkeys(VBM_COPY_OFFSETS, truncated))
        with self.assertRaisesRegex(ValueError, "truncated VBM partition table"):
            device_data.redundant_vbm_partitions(
                reader,
                VBM_COPY_OFFSETS,
                _required_partitions(),
            )


class NokiaPartiPartitionFormatTests(unittest.TestCase):
    """Protect TA-1618 compiled PartI structure without a 142 MB NAND fixture."""

    parser: ClassVar[ModuleType]

    @classmethod
    def setUpClass(cls) -> None:
        """Load the target-owned PartI parser through its normal module boundary."""
        cls.parser = _load_target_parser(
            "nokia-ta1618",
            "ta1618_device_data.py",
        )

    @staticmethod
    def _reader(entries: tuple[tuple[int, int, int, int], ...]) -> FakeNandPartitionReader:
        return FakeNandPartitionReader({NOKIA_PARTI_OFFSET: _nokia_parti_table(entries)})

    def test_valid_table_returns_ordinary_extents_without_multiplying_sentinel(self) -> None:
        """The final remainder marker is validated but is not exposed as a finite extent."""
        partitions = self.parser.parti_partitions(self._reader(_nokia_parti_entries()))

        self.assertEqual(partitions[0x10000001], (0x00080000, 0x00100000))
        self.assertEqual(partitions[0x1000000F], (0x001A0000, 0x00100000))
        self.assertEqual(partitions[0x10000018], (0x00E80000, 0x00100000))
        self.assertEqual(partitions[0x10000003], (0x013A0000, 0x00600000))
        self.assertNotIn(0x00000008, partitions)

    def test_safe_unrelated_id_and_attribute_changes_are_admitted(self) -> None:
        """A structurally equivalent ordinary record is not tied to a whole-table digest."""
        entries = list(_nokia_parti_entries())
        entries[1] = (0xABCDEF02, 0x101, 2, 2)

        partitions = self.parser.parti_partitions(self._reader(tuple(entries)))

        self.assertNotIn(0xABCDEF02, partitions)
        self.assertEqual(set(partitions), set(_required_partitions()))

    def test_invalid_shape_or_consumer_descriptor_is_rejected_precisely(self) -> None:
        """PartI must remain unique, bounded, contiguous and semantically sufficient."""
        base = list(_nokia_parti_entries())
        cases: dict[str, tuple[tuple[tuple[int, int, int, int], ...], str]] = {}

        duplicate = list(base)
        duplicate[1] = (0x00000001, 0x100, 2, 2)
        cases["duplicate ID"] = (tuple(duplicate), "duplicate TA-1618 PartI partition ID")

        missing = list(base)
        missing[8] = (0x10000099, 0x100, 116, 8)
        cases["missing required ID"] = (tuple(missing), "CM4.*is missing")

        wrong_attributes = list(base)
        wrong_attributes[8] = (0x10000018, 0x101, 116, 8)
        cases["wrong required attributes"] = (
            tuple(wrong_attributes),
            "CM4 has attributes 0x101; expected 0x100",
        )

        unsupported_attributes = list(base)
        unsupported_attributes[1] = (0x00000002, 0x102, 2, 2)
        cases["unsupported attributes"] = (
            tuple(unsupported_attributes),
            "unsupported attributes 0x102",
        )

        zero_extent = list(base)
        zero_extent[1] = (0x00000002, 0x100, 2, 0)
        cases["zero extent"] = (tuple(zero_extent), "zero-sized extent")

        overlap = list(base)
        overlap[1] = (0x00000002, 0x100, 1, 2)
        cases["overlap"] = (tuple(overlap), "starts at block 1; expected contiguous block 2")

        gap = list(base)
        gap[1] = (0x00000002, 0x100, 3, 2)
        cases["gap"] = (tuple(gap), "starts at block 3; expected contiguous block 2")

        outside = list(base)
        outside[20] = (0x10000020, 0x101, 720, 400)
        cases["outside NAND"] = (tuple(outside), "outside the 1024-block NAND")

        invalid_sentinel = list(base)
        invalid_sentinel[-1] = (0x00000008, 0x001, 881, 0xFFFFFFFF)
        cases["invalid sentinel"] = (tuple(invalid_sentinel), "invalid final remainder sentinel")

        extra_sentinel = list(base)
        extra_sentinel[20] = (0x10000020, 0x101, 720, 0xFFFFFFFF)
        cases["non-final sentinel"] = (tuple(extra_sentinel), "invalid final remainder sentinel")

        for name, (entries, error) in cases.items():
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                self.parser.parti_partitions(self._reader(entries))

    def test_table_requires_exact_count_and_complete_fixed_width_records(self) -> None:
        """A partial or differently sized compiled PartI array is not interpreted."""
        entries = _nokia_parti_entries()
        wrong_count = _nokia_parti_table(entries, declared_count=21)
        with self.assertRaisesRegex(ValueError, "partition count is 21; expected 22"):
            self.parser.parti_partitions(
                FakeNandPartitionReader({NOKIA_PARTI_OFFSET: wrong_count})
            )

        truncated = _nokia_parti_table(entries)[:-1]
        with self.assertRaisesRegex(ValueError, "truncated TA-1618 PartI partition table"):
            self.parser.parti_partitions(FakeNandPartitionReader({NOKIA_PARTI_OFFSET: truncated}))


class FixedNvFormatTests(unittest.TestCase):
    """Protect fitted-record selection from complete sorted fixed-NV streams."""

    def test_fixed_stream_uses_record_ids_and_lengths_not_fixed_offsets(self) -> None:
        """Unrelated record size and generation changes preserve selected values."""
        expected = _fixed_records()
        for unrelated in (b"odd", b"a longer unrelated value"):
            for generation in (1, 0x10203040):
                with self.subTest(unrelated=unrelated, generation=generation):
                    stream = _nv1(((2, unrelated), *expected.items()), generation)
                    self.assertEqual(
                        device_data.fixed_nv_records(stream, FIXED_RECORD_SIZES),
                        expected,
                    )

    def test_fixed_stream_rejects_incomplete_or_ambiguous_records(self) -> None:
        """Incomplete or invalid record streams do not yield partial calibration."""
        complete = _nv1(tuple(_fixed_records().items()))
        cases = {
            "missing RF record": (
                _nv1(((401, b"A" * 8), (402, b"B" * 176))),
                "fixed NV record 404 is missing or has the wrong size",
            ),
            "wrong address record length": (
                _nv1(((401, b"A" * 7), (402, b"B" * 176), (404, b"C" * 252))),
                "fixed NV record 401 is missing or has the wrong size",
            ),
            "duplicate identifier": (
                _nv1(
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
                _nv1(((402, b"B" * 176), (401, b"A" * 8), (404, b"C" * 252))),
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
        running = _running_nv()
        # These bytes resemble an ID/length but have no valid wrapper header.
        unrelated = b"\x00" * 4 + b"\x91\x01\x08\x00" + b"\x00" * 24
        bluetooth.verify_running_nv(running + unrelated + running, _fixed_records())

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
                bluetooth.verify_running_nv(data, _fixed_records())

    def test_checksum_handles_odd_bytes_and_end_around_carry(self) -> None:
        """Literal format vectors cover padding-independent checksum behavior."""
        for payload, expected in (
            (b"", 0xFFFF),
            (b"\x01\x02\x03", 0xFDFB),
            (b"\xff\xff\x01\x00", 0xFFFE),
        ):
            with self.subTest(payload=payload):
                self.assertEqual(bluetooth.nv_checksum(payload), expected)


class HeadsetGainProfileTests(unittest.TestCase):
    """Protect the compact kernel input and admitted Headset source boundaries."""

    def test_headset_app0_becomes_each_exact_profile(self) -> None:
        """Identity, source-processing status, PGA and DG enter the exact compact format."""
        cases = (
            (
                "inoi240",
                b"inoi,240-modern-4g",
                _inoi_audio_records,
                (
                    b"FPAUDIO\0"
                    b"inoi,240-modern-4g\0\0\0\0\0\0"
                    b"\x07\x00\x6c\x61\x56\x4b\x41\x37\x2d\x23\x1b"
                ),
            ),
            (
                "inoi244",
                b"inoi,244-modern-4g",
                _inoi_audio_records,
                (
                    b"FPAUDIO\0"
                    b"inoi,244-modern-4g\0\0\0\0\0\0"
                    b"\x07\x00\x6c\x61\x56\x4b\x41\x37\x2d\x23\x1b"
                ),
            ),
            (
                "ta1618",
                b"nokia,ta-1618",
                _nokia_audio_records,
                (
                    b"FPAUDIO\0"
                    b"nokia,ta-1618\0\0\0\0\0\0\0\0\0\0\0"
                    b"\x06\x01\x47\x42\x3d\x37\x32\x2c\x26\x20\x1a"
                ),
            ),
        )
        for prefix, compatible, records, expected in cases:
            with self.subTest(prefix=prefix):
                downloaded, protected = records()

                result = audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix=prefix,
                    machine_compatible=compatible,
                )

                self.assertEqual(result.prepared, {f"{prefix}-audio-profile.bin": expected})
                self.assertEqual(len(expected), 43)
                self.assertEqual(result.originals[f"{prefix}-nv425.bin"], downloaded[425])
                self.assertEqual(result.originals[f"{prefix}-nv426.bin"], downloaded[426])
                self.assertEqual(result.originals[f"{prefix}-nv440.bin"], downloaded[440])

    def test_profile_rejects_unsafe_headset_values(self) -> None:
        """A fitted profile is emitted only from matching, bounded Headset data."""
        cases: dict[str, tuple[int, int, bytes, str]] = {
            "wrong level count": (426, 62, b"\x08\x00", "expected 9"),
            "different PGA": (426, 68, b"\x06\x00", "PGA levels 1..9 differ"),
            "oversized digital gain": (426, 70, b"\x80\x00", "exceeds 127"),
            "increasing digital gain": (426, 74, b"\x6d\x00", "not monotonically"),
        }
        for name, (identifier, offset, replacement, error) in cases.items():
            downloaded, protected = _inoi_audio_records()
            changed = bytearray(downloaded[identifier])
            changed[offset : offset + len(replacement)] = replacement
            downloaded[identifier] = bytes(changed)
            if identifier in (426, 440):
                protected_changed = bytearray(protected[identifier])
                protected_changed[offset : offset + len(replacement)] = replacement
                protected[identifier] = bytes(protected_changed)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, error):
                audio_profile.prepare_headset_gain_profile(
                    downloaded,
                    protected,
                    prefix="phone",
                    machine_compatible=b"vendor,phone",
                )

        downloaded, protected = _headset_audio_records(
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
            pass_band_control=0,
        )
        with self.assertRaisesRegex(ValueError, "outside supported range 2..7"):
            audio_profile.prepare_headset_gain_profile(
                downloaded,
                protected,
                prefix="phone",
                machine_compatible=b"vendor,phone",
            )

    def test_profile_rejects_mismatched_pga_dg_and_source_processing_slices(self) -> None:
        """DownloadedNV and ProtectNV must agree on every value carried into the profile."""
        cases = (
            (426, 68, b"\x06\x00", "Headset NV426 differs"),
            (426, 70, b"\x6b\x00", "Headset NV426 differs"),
            (440, 20, b"\x01\x00", "Headset NV440 differs"),
        )
        for identifier, offset, replacement, error in cases:
            downloaded, protected = _inoi_audio_records()
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


class Cm4CompatibilityOperationTests(unittest.TestCase):
    """Check exact image admission and copy semantics with synthetic instructions."""

    @staticmethod
    def _images() -> tuple[bytes, bytes]:
        original = b"prefix" + b"\x2d\x4c" + b"middle" + b"\xe0\x6d" + b"suffix"
        expected = b"prefix" + b"\x08\xe0" + b"middle" + b"\x2b\xe0" + b"suffix"
        return original, expected

    def test_instruction_rewrite_changes_only_the_two_branches(self) -> None:
        """The synthetic output retains every byte except the two branch instructions."""
        original, expected = self._images()

        self.assertEqual(bluetooth.omit_initial_pub_policy(original, (6, 14)), expected)

    def test_revision_admits_only_the_exact_original_and_prepared_images(self) -> None:
        """Wrong source identity or output identity cannot produce an admitted image."""
        original, expected = self._images()
        revision = bluetooth.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256=hashlib.sha256(expected).hexdigest(),
            pub_policy_offsets=(6, 14),
        )

        self.assertEqual(revision.prepare(original), expected)
        for wrong in (original[:-1], original[:-1] + b"!"):
            with self.subTest(wrong=wrong), self.assertRaisesRegex(ValueError, "original CM4"):
                revision.prepare(wrong)
        wrong_output = bluetooth.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256="0" * 64,
            pub_policy_offsets=(6, 14),
        )
        with self.assertRaisesRegex(ValueError, "unexpected image"):
            wrong_output.prepare(original)
        with self.assertRaisesRegex(ValueError, "patch instructions"):
            bluetooth.omit_initial_pub_policy(bytes(22), (6, 14))


class PartitionPreparationTests(unittest.TestCase):
    """Exercise extraction and copy preservation on synthetic physical pages."""

    @staticmethod
    def _inputs(
        protected_address: bytes = b"A" * 8,
    ) -> tuple[
        device_data.PhysicalNand,
        dict[int, tuple[int, int]],
        bluetooth.Cm4Revision,
    ]:
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
        revision = bluetooth.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256=hashlib.sha256(prepared).hexdigest(),
            pub_policy_offsets=(6, 14),
        )
        return device_data.PhysicalNand(raw, 2112), partitions, revision

    def test_complete_set_keeps_original_image_and_individual_nv_bytes(self) -> None:
        """Only the prepared CM4 changes; every original remains byte-exact."""
        nand, partitions, revision = self._inputs()
        expected_originals = {
            "example-cm4.bin": b"prefix\x2d\x4cmiddle\xe0\x6dsuffix",
            "example-bt-config.bin": b"A" * 8,
            "example-bt-sprd.bin": b"B" * 176,
            "example-bt-rf-config.bin": b"C" * 252,
        }

        result = bluetooth.prepare_bluetooth_from_partitions(
            nand, partitions, prefix="example", revision=revision
        )

        self.assertEqual(result.originals, expected_originals)
        self.assertEqual(
            result.prepared,
            expected_originals | {"example-cm4.bin": b"prefix\x08\xe0middle\x2b\xe0suffix"},
        )

    def test_conflicting_protected_nv_cannot_produce_a_firmware_set(self) -> None:
        """Disagreeing fixed copies are rejected instead of selecting one address."""
        nand, partitions, revision = self._inputs(protected_address=b"D" * 8)

        with self.assertRaisesRegex(ValueError, "DownloadedNV and ProtectNV disagree"):
            bluetooth.prepare_bluetooth_from_partitions(
                nand,
                partitions,
                prefix="example",
                revision=revision,
            )


if __name__ == "__main__":
    unittest.main()

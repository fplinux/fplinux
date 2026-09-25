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

from fplinux_cli import audio_profile, device_data, fitted_device_data, fm_radio
from fplinux_cli import bluetooth_firmware as bluetooth

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
FIXED_RECORD_SIZES = {401: 8, 402: 176, 404: 252}
VBM_COPY_OFFSETS = (0x20000, 0x40000)
NOKIA_PARTI_OFFSET = 0x63DE0
INOI_HANDSFREE_VIBRATE_TONE_OFFSET = 3 * 1072 + 188
# The fitted INOI 175 Hz tone: -sin words for 24, 32 and 48 kHz, cos words for
# the same rates, then gain 0, gain 1, gain down, gain up and hold.
INOI_VIBRATE_TONE_SECTION = bytes.fromhex(
    "a1a111fd 2b23cdfd 9cb788fe d7ceee3f 2854f63f 91b3fb3f 8200 8200 0200 0800 0502"
)
# The Headfree section shared by all three fitted phones: PA word 0x001a,
# headphone PGA 7, then the digital gains of volume levels 1..9.
FITTED_HEADFREE_SECTION = bytes.fromhex("1a00 07 4e 47 3b 35 2f 2a 24 1f 1a")


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


def _add_fitted_headfree_mode(records: dict[int, bytes]) -> None:
    """Store the literal Headfree mode that all three fitted phones keep at index 1."""
    arm = bytearray(records[426])
    offset = 1072
    arm[offset : offset + 16] = b"Headfree".ljust(16, b"\0")
    struct.pack_into("<H", arm, offset + 20, 0x0032)
    struct.pack_into("<H", arm, offset + 36, 1)
    struct.pack_into("<H", arm, offset + 62, 9)
    # The fitted level-0 word precedes volume levels 1..9 and is not a volume step.
    struct.pack_into(
        "<10I",
        arm,
        offset + 64,
        0x0018009F,
        0x004E0070,
        0x00470070,
        0x003B0070,
        0x00350070,
        0x002F0070,
        0x002A0070,
        0x00240070,
        0x001F0070,
        0x001A0070,
    )
    struct.pack_into("<H", arm, offset + 466, 0x001A)
    records[426] = bytes(arm)


def _inoi_audio_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Use the literal playback-mode and vibrate-tone values of both fitted INOI phones."""
    downloaded, protected = _headset_audio_records(
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
    handsfree_levels = (
        0x00390000,
        0x00350000,
        0x00310000,
        0x002D0000,
        0x00290000,
        0x00210000,
        0x001D0000,
        0x00190000,
        0x00170000,
    )
    for records in (downloaded, protected):
        arm = bytearray(records[426])
        offset = 3 * 1072
        arm[offset : offset + 16] = b"Handsfree".ljust(16, b"\0")
        struct.pack_into("<H", arm, offset + 20, 0x0022)
        struct.pack_into("<H", arm, offset + 36, 1)
        struct.pack_into("<H", arm, offset + 62, 9)
        struct.pack_into("<9I", arm, offset + 68, *handsfree_levels)
        struct.pack_into(
            "<9H",
            arm,
            INOI_HANDSFREE_VIBRATE_TONE_OFFSET,
            *(0xFDCD, 0x232B, 0x3FF6, 0x5428, 0x0082, 0x0082, 0x0002, 0x0008, 0x0205),
        )
        struct.pack_into("<H", arm, offset + 466, 0x001A)
        records[426] = bytes(arm)
        _add_fitted_headfree_mode(records)
    return downloaded, protected


def _nokia_audio_records() -> tuple[dict[int, bytes], dict[int, bytes]]:
    """Use the fitted Nokia Headset, Headfree and speaker-only Handsfree app-0 values."""
    downloaded, protected = _headset_audio_records(
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
    handsfree_levels = (
        0x00380000,
        0x00340000,
        0x00300000,
        0x002C0000,
        0x00280000,
        0x00240000,
        0x00200000,
        0x001C0000,
        0x001B0000,
    )
    for records in (downloaded, protected):
        arm = bytearray(records[426])
        arm[3 * 1072 : 3 * 1072 + 16] = b"Handsfree".ljust(16, b"\0")
        struct.pack_into("<H", arm, 3 * 1072 + 20, 0x0022)
        struct.pack_into("<H", arm, 3 * 1072 + 36, 1)
        struct.pack_into("<H", arm, 3 * 1072 + 62, 9)
        struct.pack_into("<9I", arm, 3 * 1072 + 68, *handsfree_levels)
        struct.pack_into("<H", arm, 3 * 1072 + 466, 0x001A)
        records[426] = bytes(arm)
        _add_fitted_headfree_mode(records)
    return downloaded, protected


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


class FmRadioConfigTests(unittest.TestCase):
    """Protect the fitted FM payload consumed by the radio ENABLE operation."""

    def test_matching_nv419_becomes_the_exact_normalized_fm_payload(self) -> None:
        """ENABLE zeros the first word and duplicates th1 while preserving other bytes."""
        original = bytes(range(128))
        stream = _nv1(((419, original),))
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
        self.assertEqual(result.originals, {"phone-nv419.bin": original})
        self.assertEqual(result.prepared, {"phone-fm-config.bin": expected})
        self.assertEqual(len(expected), 128)

    def test_missing_wrong_size_or_conflicting_fm_record_cannot_produce_a_payload(self) -> None:
        """Incomplete or disagreeing fixed copies cannot become fitted FM input."""
        for records in ((), ((419, bytes(127)),)):
            with (
                self.subTest(records=records),
                self.assertRaisesRegex(
                    ValueError, "fixed NV record 419 is missing or has the wrong size"
                ),
            ):
                device_data.fixed_nv_records(_nv1(records), {419: 128})

        original = bytes(range(128))
        different = bytearray(original)
        different[64] ^= 1
        with self.assertRaisesRegex(ValueError, "NV419 differs"):
            fm_radio.prepare_fm_config(
                {419: original},
                {419: bytes(different)},
                prefix="phone",
            )


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
    """Protect the compact kernel input and admitted fitted playback gains."""

    def test_fitted_playback_modes_become_each_exact_profile(self) -> None:
        """Target capabilities select the literal compact playback payload."""
        cases = (
            (
                "inoi240",
                b"inoi,240-modern-4g",
                _inoi_audio_records,
                True,
                (
                    b"FPAUDIO\0"
                    b"inoi,240-modern-4g\0\0\0\0\0\0"
                    b"\x07\x00\x6c\x61\x56\x4b\x41\x37\x2d\x23\x1b"
                    b"\x1a\x00\x39\x35\x31\x2d\x29\x21\x1d\x19\x17"
                    b"\x1a\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a"
                )
                + INOI_VIBRATE_TONE_SECTION,
            ),
            (
                "inoi244",
                b"inoi,244-modern-4g",
                _inoi_audio_records,
                True,
                (
                    b"FPAUDIO\0"
                    b"inoi,244-modern-4g\0\0\0\0\0\0"
                    b"\x07\x00\x6c\x61\x56\x4b\x41\x37\x2d\x23\x1b"
                    b"\x1a\x00\x39\x35\x31\x2d\x29\x21\x1d\x19\x17"
                    b"\x1a\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a"
                )
                + INOI_VIBRATE_TONE_SECTION,
            ),
            (
                "ta1618",
                b"nokia,ta-1618",
                _nokia_audio_records,
                False,
                (
                    b"FPAUDIO\0"
                    b"nokia,ta-1618\0\0\0\0\0\0\0\0\0\0\0"
                    b"\x06\x01\x47\x42\x3d\x37\x32\x2c\x26\x20\x1a"
                    b"\x1a\x00\x38\x34\x30\x2c\x28\x24\x20\x1c\x1b"
                    b"\x1a\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a"
                ),
            ),
        )
        for prefix, compatible, records, speaker_vibration, expected in cases:
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
                self.assertEqual(result.originals[f"{prefix}-nv425.bin"], downloaded[425])
                self.assertEqual(result.originals[f"{prefix}-nv426.bin"], downloaded[426])
                self.assertEqual(result.originals[f"{prefix}-nv440.bin"], downloaded[440])

    def test_speaker_profiles_select_handsfree_and_headfree_by_name_after_reordering(
        self,
    ) -> None:
        """Each speaker section comes from its named mode, not from a fixed mode index."""
        downloaded, protected = _nokia_audio_records()
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
            result.prepared["ta1618-audio-profile.bin"][43:],
            b"\x1a\x00\x38\x34\x30\x2c\x28\x24\x20\x1c\x1b"
            b"\x06\x00\x07\x4e\x47\x3b\x35\x2f\x2a\x24\x1f\x1a",
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
            downloaded, protected = _nokia_audio_records()
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
            downloaded, protected = _nokia_audio_records()
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
            downloaded, protected = _inoi_audio_records()
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
        downloaded, protected = _inoi_audio_records()
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
        protected_fm: bytes = bytes(range(128)),
    ) -> tuple[
        device_data.PhysicalNand,
        dict[int, tuple[int, int]],
        bluetooth.Cm4Revision,
    ]:
        original = b"prefix\x2d\x4cmiddle\xe0\x6dsuffix"
        prepared = b"prefix\x08\xe0middle\x2b\xe0suffix"
        downloaded_audio, protected_audio = _inoi_audio_records()
        downloaded_records = _fixed_records() | {419: bytes(range(128))} | downloaded_audio
        protected_records = {
            401: protected_address,
            402: b"B" * 176,
            404: b"C" * 252,
            419: protected_fm,
        } | protected_audio
        payloads = (
            original,
            _nv1(tuple(downloaded_records.items())),
            _nv1(tuple(protected_records.items())),
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

    def test_shared_fitted_extraction_includes_fm_and_rejects_conflicting_copies(self) -> None:
        """Every fitted target needs the same admitted FM group from matching NV419 copies."""
        nand, partitions, revision = self._inputs()

        result = fitted_device_data.prepare_from_partitions(
            nand,
            partitions,
            prefix="phone",
            revision=revision,
            machine_compatible=b"vendor,phone",
        )

        self.assertEqual(set(result.groups), {"bluetooth", "audio-profile", "fm-radio"})
        self.assertEqual(
            result.groups["fm-radio"].originals, {"phone-nv419.bin": bytes(range(128))}
        )
        self.assertEqual(
            result.groups["fm-radio"].prepared["phone-fm-config.bin"][:18],
            bytes.fromhex("00 00 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 0e 0f"),
        )

        conflicting = bytes(range(127)) + b"\0"
        nand, partitions, revision = self._inputs(protected_fm=conflicting)
        with self.assertRaisesRegex(ValueError, "NV419 differs"):
            fitted_device_data.prepare_from_partitions(
                nand,
                partitions,
                prefix="phone",
                revision=revision,
                machine_compatible=b"vendor,phone",
            )

    def test_speaker_vibration_target_receives_the_vibrate_tone_section(self) -> None:
        """Only a target that vibrates through its speaker gets the tone after Headfree."""
        for speaker_vibration, expected_tail in ((False, b""), (True, INOI_VIBRATE_TONE_SECTION)):
            nand, partitions, revision = self._inputs()

            result = fitted_device_data.prepare_from_partitions(
                nand,
                partitions,
                prefix="phone",
                revision=revision,
                machine_compatible=b"vendor,phone",
                speaker_vibration=speaker_vibration,
            )

            profile = result.groups["audio-profile"].prepared["phone-audio-profile.bin"]
            with self.subTest(speaker_vibration=speaker_vibration):
                self.assertEqual(profile[54:], FITTED_HEADFREE_SECTION + expected_tail)

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

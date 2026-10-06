# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data partitions scenarios."""

from __future__ import annotations

from typing import TYPE_CHECKING, ClassVar

import pytest
from fplinux_cli.device_data import formats as device_data

from tests.small.device_data.partition_fixtures import (
    NOKIA_PARTI_OFFSET,
    VBM_COPY_OFFSETS,
    FakeNandPartitionReader,
    load_target_parser,
    nokia_parti_entries,
    nokia_parti_table,
    required_partitions,
    vbm_entries,
    vbm_table,
)

if TYPE_CHECKING:
    from types import ModuleType


class VbmPartitionFormatTests:
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
                VBM_COPY_OFFSETS[0]: vbm_table(
                    first_entries,
                    peer_fields=b"first-copy",
                ),
                VBM_COPY_OFFSETS[1]: vbm_table(
                    second_entries,
                    peer_fields=b"secondcopy",
                ),
            }
        )

    def test_matching_copies_admit_only_named_partitions_and_ignore_peer_fields(self) -> None:
        """Different copy metadata does not hide equal decoded consumer descriptors."""
        partitions = device_data.redundant_vbm_partitions(
            self._reader(vbm_entries()),
            VBM_COPY_OFFSETS,
            required_partitions(),
        )

        assert (partitions) == (
            {
                0x10000001: (0x00080000, 0x00100000),
                0x1000000F: (0x001A0000, 0x00100000),
                0x10000018: (0x00E80000, 0x00100000),
                0x10000003: (0x013A0000, 0x00600000),
            }
        )

    def test_agreed_safe_table_changes_are_not_rejected_as_unknown_images(self) -> None:
        """An unrelated attribute and a required location may change when both copies agree."""
        entries = list(vbm_entries())
        entries[3] = (0x10000018, 0x100, 124, 131)
        entries[-1] = (0x00000099, 0xBEEF, 701, 980)

        partitions = device_data.redundant_vbm_partitions(
            self._reader(tuple(entries)),
            VBM_COPY_OFFSETS,
            required_partitions(),
        )

        assert (partitions[0x10000018]) == ((0x00F80000, 0x00100000))
        assert (0x00000099) not in (partitions)

    def test_copies_must_have_identical_decoded_counts_and_entries(self) -> None:
        """One changed record is ambiguous even when both copies remain individually safe."""
        second_entries = list(vbm_entries())
        second_entries[-1] = (0x00000008, 0x0101, 701, 980)

        with pytest.raises(ValueError, match=r"redundant VBM.*disagree"):
            device_data.redundant_vbm_partitions(
                self._reader(vbm_entries(), tuple(second_entries)),
                VBM_COPY_OFFSETS,
                required_partitions(),
            )

    @pytest.mark.parametrize(
        "case",
        [
            "duplicate ID",
            "missing required ID",
            "overlapping extents",
            "zero or reversed extent",
            "outside NAND",
            "wrong required attributes",
        ],
        ids=[
            "duplicate",
            "missing-required",
            "overlap",
            "reversed",
            "outside",
            "wrong-attributes",
        ],
    )
    def test_structural_or_required_partition_damage_is_rejected_precisely(
        self, case: str
    ) -> None:
        """Malformed extents and missing consumer semantics cannot reach extraction."""
        base = list(vbm_entries())
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

        entries, error = cases[case]
        with pytest.raises(ValueError, match=error):
            device_data.redundant_vbm_partitions(
                self._reader(entries),
                VBM_COPY_OFFSETS,
                required_partitions(),
            )

    @pytest.mark.parametrize(
        "count", [pytest.param(0, id="empty-table"), pytest.param(101, id="count-above-bound")]
    )
    def test_declared_count_is_bounded_and_complete_records_are_required(self, count: int) -> None:
        """The count cannot trigger an unbounded read or admit a truncated final record."""
        table = vbm_table((), declared_count=count)
        reader = FakeNandPartitionReader(dict.fromkeys(VBM_COPY_OFFSETS, table))
        with pytest.raises(ValueError, match=f"partition count {count}.*bound"):
            device_data.redundant_vbm_partitions(
                reader,
                VBM_COPY_OFFSETS,
                required_partitions(),
            )

        truncated = vbm_table(vbm_entries())[:-1]
        reader = FakeNandPartitionReader(dict.fromkeys(VBM_COPY_OFFSETS, truncated))
        with pytest.raises(ValueError, match="truncated VBM partition table"):
            device_data.redundant_vbm_partitions(
                reader,
                VBM_COPY_OFFSETS,
                required_partitions(),
            )


class NokiaPartiPartitionFormatTests:
    """Protect TA-1618 compiled PartI structure without a 142 MB NAND fixture."""

    parser: ClassVar[ModuleType]

    @pytest.fixture(scope="class")
    @classmethod
    def parti_parser(cls) -> ModuleType:
        """Load the target-owned PartI parser through its normal module boundary."""
        cls.parser = load_target_parser(
            "nokia-ta1618",
            "ta1618_device_data.py",
        )
        return cls.parser

    @staticmethod
    def _reader(entries: tuple[tuple[int, int, int, int], ...]) -> FakeNandPartitionReader:
        return FakeNandPartitionReader({NOKIA_PARTI_OFFSET: nokia_parti_table(entries)})

    def test_valid_table_returns_ordinary_extents_without_multiplying_sentinel(
        self, parti_parser: ModuleType
    ) -> None:
        """The final remainder marker is validated but is not exposed as a finite extent."""
        partitions = parti_parser.parti_partitions(self._reader(nokia_parti_entries()))

        assert (partitions[0x10000001]) == ((0x00080000, 0x00100000))
        assert (partitions[0x1000000F]) == ((0x001A0000, 0x00100000))
        assert (partitions[0x10000018]) == ((0x00E80000, 0x00100000))
        assert (partitions[0x10000003]) == ((0x013A0000, 0x00600000))
        assert (0x00000008) not in (partitions)

    def test_safe_unrelated_id_and_attribute_changes_are_admitted(
        self, parti_parser: ModuleType
    ) -> None:
        """A structurally equivalent ordinary record is not tied to a whole-table digest."""
        entries = list(nokia_parti_entries())
        entries[1] = (0xABCDEF02, 0x101, 2, 2)

        partitions = parti_parser.parti_partitions(self._reader(tuple(entries)))

        assert (0xABCDEF02) not in (partitions)
        assert (set(partitions)) == (set(required_partitions()))

    @pytest.mark.parametrize(
        "case",
        [
            "duplicate ID",
            "missing required ID",
            "wrong required attributes",
            "unsupported attributes",
            "zero extent",
            "overlap",
            "gap",
            "outside NAND",
            "invalid sentinel",
            "non-final sentinel",
        ],
        ids=[
            "duplicate",
            "missing-required",
            "wrong-attributes",
            "unsupported-attributes",
            "zero-extent",
            "overlap",
            "gap",
            "outside",
            "invalid-sentinel",
            "early-sentinel",
        ],
    )
    def test_invalid_shape_or_consumer_descriptor_is_rejected_precisely(
        self, parti_parser: ModuleType, case: str
    ) -> None:
        """PartI must remain unique, bounded, contiguous and semantically sufficient."""
        base = list(nokia_parti_entries())
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

        entries, error = cases[case]
        with pytest.raises(ValueError, match=error):
            parti_parser.parti_partitions(self._reader(entries))

    def test_table_requires_exact_count_and_complete_fixed_width_records(
        self, parti_parser: ModuleType
    ) -> None:
        """A partial or differently sized compiled PartI array is not interpreted."""
        entries = nokia_parti_entries()
        wrong_count = nokia_parti_table(entries, declared_count=21)
        with pytest.raises(ValueError, match="partition count is 21; expected 22"):
            parti_parser.parti_partitions(
                FakeNandPartitionReader({NOKIA_PARTI_OFFSET: wrong_count})
            )

        truncated = nokia_parti_table(entries)[:-1]
        with pytest.raises(ValueError, match="truncated TA-1618 PartI partition table"):
            parti_parser.parti_partitions(FakeNandPartitionReader({NOKIA_PARTI_OFFSET: truncated}))

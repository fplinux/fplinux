# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data partitions scenarios."""

from __future__ import annotations

import pytest
from fplinux_cli.device_data import formats as device_data
from fplinux_cli.device_data.partitions import discover_partitions

from tests.small.device_data.partition_fixtures import (
    VBM_COPY_OFFSETS,
    FakeNandPartitionReader,
    parti_entries,
    parti_table,
    required_partitions,
    vbm_entries,
    vbm_table,
)


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
    def test_declared_vbm_count_is_bounded(self, count: int) -> None:
        """The count cannot trigger an unbounded read or admit an empty table."""
        table = vbm_table((), declared_count=count)
        reader = FakeNandPartitionReader(dict.fromkeys(VBM_COPY_OFFSETS, table))
        with pytest.raises(ValueError, match=f"partition count {count}.*bound"):
            device_data.redundant_vbm_partitions(
                reader,
                VBM_COPY_OFFSETS,
                required_partitions(),
            )


class PartitionDiscoveryTests:
    """Exercise table discovery through sparse main-area reads and table admission."""

    @pytest.mark.parametrize("offset", [0x840, 0x26120, 0x1FFF0])
    def test_parti_relocation_returns_only_required_extents(self, offset: int) -> None:
        """Word-aligned tables may move, including across an erase-block boundary."""
        reader = FakeNandPartitionReader({offset: parti_table(parti_entries())}, block_count=32)

        table = discover_partitions(reader, required_partitions())

        assert table.kind == "PartI"
        assert table.extents == {
            0x10000001: (0x00040000, 0x00040000),
            0x1000000F: (0x00080000, 0x00020000),
            0x10000018: (0x000A0000, 0x00060000),
            0x10000003: (0x00100000, 0x00040000),
        }

    def test_parti_unrelated_records_sizes_and_count_may_change(self) -> None:
        """Consumer semantics survive insertion and changed nonconsumer attributes."""
        entries = (
            (0xABCDEF00, 0xBEEF, 0, 1),
            (0xABCDEF01, 0x102, 1, 2),
            (0x10000001, 0x100, 3, 2),
            (0x1000000F, 0x100, 5, 1),
            (0x10000018, 0x100, 6, 3),
            (0x10000003, 0x001, 9, 2),
            (0xABCDEF08, 0x246, 11, 0xFFFFFFFF),
        )
        reader = FakeNandPartitionReader({0x308: parti_table(entries)}, block_count=32)

        table = discover_partitions(reader, required_partitions())

        assert table.extents == {
            0x10000001: (0x00060000, 0x00040000),
            0x1000000F: (0x000A0000, 0x00020000),
            0x10000018: (0x000C0000, 0x00060000),
            0x10000003: (0x00120000, 0x00040000),
        }

    def test_parti_remainder_is_bounded_when_requested(self) -> None:
        """The remainder marker yields its actual finite capacity for a named consumer."""
        reader = FakeNandPartitionReader({0x100: parti_table(parti_entries())}, block_count=32)

        table = discover_partitions(reader, {8: device_data.RequiredPartition("storage", 1)})

        assert table.extents == {8: (0x00140000, 0x002C0000)}

    def test_identical_parti_tables_are_consistent(self) -> None:
        """Repeated equal tables identify the same physical extent."""
        encoded = parti_table(parti_entries())
        reader = FakeNandPartitionReader({0x100: encoded, 0x504: encoded}, block_count=32)

        table = discover_partitions(
            reader, {0x10000018: device_data.RequiredPartition("CM4", 256)}
        )

        assert table.extents == {0x10000018: (0x000A0000, 0x00060000)}

    def test_disagreeing_valid_parti_tables_are_rejected_before_consumer_selection(self) -> None:
        """Even a nonconsumer disagreement leaves the physical layout ambiguous."""
        changed = list(parti_entries())
        changed[0] = (0xABCDEF00, 0x100, 0, 2)
        reader = FakeNandPartitionReader(
            {0x100: parti_table(parti_entries()), 0x504: parti_table(tuple(changed))},
            block_count=32,
        )

        with pytest.raises(ValueError, match="PartI partition tables that disagree"):
            discover_partitions(reader, required_partitions())

    @pytest.mark.parametrize(
        ("index", "replacement"),
        [
            pytest.param(1, (1, 0x100, 2, 2), id="duplicate-id"),
            pytest.param(1, (0x10000001, 0x100, 2, 0), id="zero-size"),
            pytest.param(2, (0x1000000F, 0x100, 3, 1), id="overlap"),
            pytest.param(2, (0x1000000F, 0x100, 5, 1), id="gap"),
            pytest.param(4, (0x10000003, 1, 8, 25), id="outside-nand"),
            pytest.param(4, (0x10000003, 1, 8, 24), id="empty-remainder"),
            pytest.param(1, (0x10000001, 0x100, 2, 0xFFFFFFFF), id="early-remainder"),
            pytest.param(5, (8, 1, 11, 0xFFFFFFFF), id="wrong-remainder-start"),
            pytest.param(5, (8, 1, 10, 1), id="missing-remainder"),
        ],
    )
    def test_malformed_parti_layout_is_not_admitted(
        self, index: int, replacement: tuple[int, int, int, int]
    ) -> None:
        """Duplicate IDs, gaps and unsafe bounds cannot select physical data."""
        entries = list(parti_entries())
        entries[index] = replacement
        reader = FakeNandPartitionReader({0x100: parti_table(tuple(entries))}, block_count=32)

        with pytest.raises(ValueError, match="no VBM or PartI partition table found"):
            discover_partitions(reader, required_partitions())

    @pytest.mark.parametrize(
        ("replacement", "error"),
        [
            ((0x10000099, 0x100, 5, 3), "CM4.*is missing"),
            ((0x10000018, 0x101, 5, 3), "CM4 has attributes 0x101; expected 0x100"),
        ],
        ids=["missing-consumer", "wrong-consumer-attributes"],
    )
    def test_valid_parti_table_must_satisfy_caller_requirements(
        self, replacement: tuple[int, int, int, int], error: str
    ) -> None:
        """A discovered layout cannot substitute a missing or incompatible consumer."""
        entries = list(parti_entries())
        entries[3] = replacement
        reader = FakeNandPartitionReader({0x100: parti_table(tuple(entries))}, block_count=32)

        with pytest.raises(ValueError, match=error):
            discover_partitions(reader, required_partitions())

    @pytest.mark.parametrize("count", [0, 1, 33, 101])
    def test_parti_count_must_be_bounded_and_describe_the_complete_table(self, count: int) -> None:
        """The count must admit all records within the format and capacity bounds."""
        reader = FakeNandPartitionReader(
            {0x100: parti_table(parti_entries(), declared_count=count)}, block_count=32
        )

        with pytest.raises(ValueError, match="no VBM or PartI partition table found"):
            discover_partitions(reader, required_partitions())

    @pytest.mark.parametrize("offset", [0x101, 0x3FFFEC])
    def test_unaligned_or_capacity_truncated_parti_is_not_admitted(self, offset: int) -> None:
        """Compiled tables require word alignment and all bytes inside NAND."""
        reader = FakeNandPartitionReader({offset: parti_table(parti_entries())}, block_count=32)

        with pytest.raises(ValueError, match="no VBM or PartI partition table found"):
            discover_partitions(reader, required_partitions())

    def test_erased_nand_reports_missing_tables(self) -> None:
        """An image without either format produces a clear discovery failure."""
        with pytest.raises(ValueError, match="no VBM or PartI partition table found"):
            discover_partitions(FakeNandPartitionReader({}, block_count=32), required_partitions())

    def test_vbm_copies_are_discovered_at_relocated_block_starts(self) -> None:
        """Magic and agreeing descriptors determine locations without fixed offsets."""
        entries = ((0x10000018, 0x100, 5, 7), (8, 1, 10, 20))
        reader = FakeNandPartitionReader(
            {0xE0000: vbm_table(entries), 0x1E0000: vbm_table(entries)}, block_count=32
        )

        table = discover_partitions(
            reader, {0x10000018: device_data.RequiredPartition("CM4", 256)}
        )

        assert table.kind == "VBM"
        assert table.extents == {0x10000018: (0x000A0000, 0x00060000)}

    @pytest.mark.parametrize("offsets", [(0x20000,), (0x20000, 0x40000, 0x60000)])
    def test_vbm_requires_exactly_two_discovered_copies(self, offsets: tuple[int, ...]) -> None:
        """Incomplete or extra redundancy cannot determine an admitted VBM table."""
        encoded = vbm_table(((0x10000018, 0x100, 5, 7),))
        reader = FakeNandPartitionReader(dict.fromkeys(offsets, encoded), block_count=32)

        with pytest.raises(ValueError, match=f"expected two VBM.*found {len(offsets)}"):
            discover_partitions(reader, required_partitions())

    def test_vbm_disagreement_is_not_hidden_by_valid_parti(self) -> None:
        """A damaged redundant table cannot fall back to a compiled table."""
        reader = FakeNandPartitionReader(
            {
                0x20000: vbm_table(((0x10000018, 0x100, 5, 7),)),
                0x40000: vbm_table(((0x10000018, 0x100, 6, 8),)),
                0x104: parti_table(parti_entries()),
            },
            block_count=32,
        )

        with pytest.raises(ValueError, match="redundant VBM partition tables disagree"):
            discover_partitions(reader, {0x10000018: device_data.RequiredPartition("CM4", 256)})

    @pytest.mark.parametrize(
        ("fragments", "bad_block"),
        [
            (
                {0x20000: vbm_table(((1, 0x100, 3, 4),)), 0x40000: vbm_table(((1, 0x100, 3, 4),))},
                1,
            ),
            ({0x1FFF0: parti_table(parti_entries())}, 1),
        ],
        ids=["vbm-copy", "parti-crossing-block-boundary"],
    )
    def test_discovered_tables_must_pass_selected_block_admission(
        self, fragments: dict[int, bytes], bad_block: int
    ) -> None:
        """Discovery reads do not bypass admissibility of the selected table blocks."""
        reader = FakeNandPartitionReader(
            fragments, block_count=32, bad_blocks=frozenset({bad_block})
        )

        with pytest.raises(ValueError, match=f"selected NAND block {bad_block} is marked bad"):
            discover_partitions(reader, {1: device_data.RequiredPartition("boot", 256)})

# SPDX-License-Identifier: GPL-2.0-only
"""Decode synthetic UMS9117 partition, keypad and NAND records in process."""

from __future__ import annotations

import struct

import pytest

from tests.fixtures.stock_board_report import (
    APPLICATION_OFFSET,
    BLOCK_MAIN_BYTES,
    EXPECTED_21E5,
    NAND_RECORD_21E5,
    NAND_TERMINATOR,
    SPARSE_KEYMAP,
    SPARSE_MATRIX,
    STOCK_IMAGE,
    VBM_ENTRIES,
    agreeing_vbm,
    backup,
    keymap,
    signed_image,
    vbm_copy,
)

PARTI_ENTRIES = (
    (0x00000001, 0x100, 0, 2),
    (0x00000003, 0x100, 2, 4),
    (0x10000001, 0x100, 6, 2),
    (0x00000008, 0x001, 8, 0xFFFFFFFF),
)
# A second chip ID with 2048 blocks; its other fields stay zero.
NAND_RECORD_B1A1 = bytes.fromhex("a1b1000000000000 00080000") + bytes(0x38 - 12)


def _parti_table(entries: tuple[tuple[int, int, int, int], ...]) -> bytes:
    """Encode the PartI count and (id, attributes, first block, block count) records."""
    return struct.pack("<I", len(entries)) + b"".join(
        struct.pack("<4I", *entry) for entry in entries
    )


class PartitionDiscoveryTests:
    """Find the stock application image by searching, not by fixed table offsets."""

    def test_vbm_copies_at_the_end_select_the_signed_image_only(self) -> None:
        """The erased rest of the partition is not passed on as stock image data."""
        image = signed_image(b"stock application" * 64)

        found = STOCK_IMAGE.find_application_image(backup(image, vbm=agreeing_vbm()))

        assert (found.contents) == (image)
        assert (found.nand_offset) == (APPLICATION_OFFSET)
        assert (found.partition_table) == ("VBM")

    def test_parti_table_anywhere_selects_the_same_image(self) -> None:
        """A PartI table across a page boundary, away from any block start, is found."""
        image = signed_image(b"stock application" * 64)

        found = STOCK_IMAGE.find_application_image(
            backup(image, parti=(0x7FC, _parti_table(PARTI_ENTRIES)))
        )

        assert (found.contents) == (image)
        assert (found.nand_offset) == (APPLICATION_OFFSET)
        assert (found.partition_table) == ("PartI")

    def test_disagreeing_vbm_copies_are_refused(self) -> None:
        """Two readable but different copies leave the layout ambiguous."""
        changed = (*VBM_ENTRIES[:1], (0x00000003, 0x100, 2, 4), *VBM_ENTRIES[2:])
        vbm = (vbm_copy(VBM_ENTRIES, b"first-copy"), vbm_copy(changed, b"secondcopy"))

        with pytest.raises(ValueError, match=r"VBM.*disagree"):
            STOCK_IMAGE.find_application_image(backup(signed_image(b"image"), vbm=vbm))

    @pytest.mark.parametrize(
        "case",
        [
            "no table",
            "one VBM copy",
            "no application entry",
            "no signed image",
            "image larger than its partition",
        ],
        ids=["no-table", "one-vbm-copy", "no-application", "unsigned-image", "oversized-image"],
    )
    def test_missing_tables_or_image_are_refused(self, case: str) -> None:
        """No table, one VBM copy, no application entry or no signed image stops extraction."""
        image = signed_image(b"image")
        without_application = tuple(
            (0x00000004, *entry[1:]) if entry[0] == 3 else entry for entry in PARTI_ENTRIES
        )
        oversized = bytearray(image)
        oversized[0x30:0x34] = struct.pack("<I", 4 * BLOCK_MAIN_BYTES)
        cases = {
            "no table": (backup(image), "no VBM or PartI partition table"),
            "one VBM copy": (
                backup(image, vbm=agreeing_vbm()[:1]),
                "expected two VBM partition-table copies, found 1",
            ),
            "no application entry": (
                backup(image, parti=(0x1000, _parti_table(without_application))),
                "no stock application image partition",
            ),
            "no signed image": (
                backup(b"\x00" * 0x400, vbm=agreeing_vbm()),
                "no DHTB header",
            ),
            "image larger than its partition": (
                backup(bytes(oversized), vbm=agreeing_vbm()),
                "does not fit",
            ),
        }
        nand, message = cases[case]
        with pytest.raises(ValueError, match=message):
            STOCK_IMAGE.find_application_image(nand)


class KeypadTests:
    """Turn the stock keymap into the matrix, boot key and EIC9 candidate."""

    def test_matrix_positions_follow_the_column_major_keymap_with_a_hole(self) -> None:
        """Entry column * 8 + row is MATRIX_KEY(row, column); an empty entry is no key."""
        report = STOCK_IMAGE.keypad_report(keymap(SPARSE_KEYMAP))

        assert (report["matrix"]) == (SPARSE_MATRIX)
        assert ((report["rows"], report["columns"])) == ((3, 2))
        assert (report["boot_key"]) == ({"code": "0x2a", "key": "FPLINUX_KEY_STAR"})

    def test_only_a_single_missing_needed_key_is_the_eic9_candidate(self) -> None:
        """One absent key is proposed for EIC9; with two absent keys none is proposed."""
        # Column 0 holds the digits 0-7, columns 1 and 2 the other keys except 8.
        keys_without_8 = {row: 0x30 + row for row in range(8)} | {
            8: 0x39,
            9: 0x2A,
            10: 0x23,
            11: 0x01,
            12: 0x04,
            13: 0x05,
            14: 0x06,
            15: 0x07,
            16: 0x08,
            17: 0x09,
            18: 0x0D,
        }
        one_missing = STOCK_IMAGE.keypad_report(keymap(keys_without_8))
        two_missing = STOCK_IMAGE.keypad_report(keymap(keys_without_8 | {18: 0xFFFF}))

        eight = {"code": "0x38", "key": "FPLINUX_KEY_8"}
        assert (one_missing["eic9_candidate"]) == (eight)
        assert (one_missing["missing_keys"]) == ([eight])
        assert (two_missing["eic9_candidate"]) is None
        assert (two_missing["missing_keys"]) == (
            [{"code": "0x0d", "key": "FPLINUX_KEY_OK"}, eight]
        )


class NandConfigTests:
    """Decode the stock NAND configuration table with its documented record layout."""

    def test_records_are_decoded_up_to_the_terminating_record(self) -> None:
        """Bytes after the terminator are not read as further chips."""
        table = NAND_RECORD_21E5 + NAND_RECORD_B1A1 + NAND_TERMINATOR + NAND_RECORD_21E5

        records = STOCK_IMAGE.decode_nand_configs(table)

        assert (len(records)) == (2)
        assert (records[0]) == (EXPECTED_21E5)
        assert ((records[1]["id"], records[1]["block_count"])) == (("0xb1a1", 2048))

    def test_table_without_a_terminating_record_is_refused(self) -> None:
        """A truncated table cannot be reported as complete."""
        with pytest.raises(ValueError, match="no terminating record"):
            STOCK_IMAGE.decode_nand_configs(NAND_RECORD_21E5 + NAND_RECORD_B1A1)

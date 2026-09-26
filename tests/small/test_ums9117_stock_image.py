# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic checks of UMS9117 board-map extraction; no stock image or phone backup."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.device_data import PhysicalNand, PreparedGroup

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]
FAKE_TOOL = ROOT / "tests/fixtures/fphelper/fake_fphelper.py"

PAGE_MAIN_BYTES = 2048
BLOCK_MAIN_BYTES = 64 * PAGE_MAIN_BYTES
BLOCKS = 16
# Every synthetic layout keeps the stock application image in blocks 2-5.
APPLICATION_OFFSET = 2 * BLOCK_MAIN_BYTES
VBM_ENTRIES = (
    (0x00000001, 0x100, 0, 1),
    (0x00000003, 0x100, 2, 5),
    (0x10000001, 0x100, 6, 7),
    (0x00000008, 0x001, 8, 13),
)
PARTI_ENTRIES = (
    (0x00000001, 0x100, 0, 2),
    (0x00000003, 0x100, 2, 4),
    (0x10000001, 0x100, 6, 2),
    (0x00000008, 0x001, 8, 0xFFFFFFFF),
)

# One NAND configuration record, field by field at its documented offsets.
NAND_RECORD_21E5 = bytes.fromhex(
    "e521000000000000"  # 0x00 chip ID
    "00040000"  # 0x08 1024 blocks
    "01000000"  # 0x0c one plane
    "2b000000"  # 0x10 43 reserved blocks
    "0004 2000"  # 0x14 1024-byte sectors, 32 spare bytes per sector
    "0200 4000"  # 0x18 two sectors per page, 64 pages per block
    "01000000"  # 0x1c write count 1, bad page 0, bad sector 0, bad marker position 0
    "01ff0004"  # 0x20 bad marker length 1, safe data position -1, length 0, main ECC position 4
    "0c040101"  # 0x24 main ECC 12 bits, risk 4, spare ECC position 1, 1 bit
    "01000501"  # 0x28 spare ECC risk 1, bus width 0, address cycles 5, advance 1
    "14002c01"  # 0x2c tr 20, tw 300
    "d0075000"  # 0x30 te 2000, tf 80
    "00000000"  # 0x34 padding
)
# A second chip ID with 2048 blocks; its other fields stay zero.
NAND_RECORD_B1A1 = bytes.fromhex("a1b1000000000000 00080000") + bytes(0x38 - 12)
NAND_TERMINATOR = bytes(0x21) + b"\xff" + bytes(0x38 - 0x22)
EXPECTED_21E5 = {
    "id": "0x21e5",
    "block_count": 1024,
    "plane_count": 1,
    "reserved_block_count": 43,
    "sector_bytes": 1024,
    "spare_bytes_per_sector": 32,
    "sectors_per_page": 2,
    "pages_per_block": 64,
    "write_count_per_page": 1,
    "bad_page_index": 0,
    "bad_sector_index": 0,
    "bad_marker_position": 0,
    "bad_marker_length": 1,
    "safe_data_position": -1,
    "safe_data_length": 0,
    "main_ecc_position": 4,
    "main_ecc_bits": 12,
    "main_ecc_risk_bits": 4,
    "spare_ecc_position": 1,
    "spare_ecc_bits": 1,
    "spare_ecc_risk_bits": 1,
    "bus_width": 0,
    "address_cycles": 5,
    "advance": 1,
    "tr_time": 20,
    "tw_time": 300,
    "te_time": 2000,
    "tf_time": 80,
}


def _load_stock_image() -> ModuleType:
    path = ROOT / "platforms/ums9117/host/stock_image.py"
    spec = importlib.util.spec_from_file_location("ums9117_stock_image_under_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load the platform module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


STOCK_IMAGE = _load_stock_image()


def _signed_image(payload: bytes) -> bytes:
    """Prefix the DHTB header: magic, version 1 and the image size at 0x30."""
    header = bytearray(0x200)
    header[0:8] = b"DHTB" + struct.pack("<I", 1)
    header[0x30:0x34] = struct.pack("<I", len(payload))
    return bytes(header) + payload


def _vbm_copy(entries: tuple[tuple[int, int, int, int], ...], peer: bytes) -> bytes:
    """Encode the VBM header, count and inclusive (id, attributes, first, last) records."""
    return b"".join(
        (
            b"VBM_BOOT",
            struct.pack("<I", 0x102),
            peer,
            struct.pack("<H", len(entries)),
            *(struct.pack("<IHHH", *entry) for entry in entries),
        )
    )


def _parti_table(entries: tuple[tuple[int, int, int, int], ...]) -> bytes:
    """Encode the PartI count and (id, attributes, first block, block count) records."""
    return struct.pack("<I", len(entries)) + b"".join(
        struct.pack("<4I", *entry) for entry in entries
    )


def _backup(
    image: bytes,
    *,
    vbm: tuple[bytes, ...] = (),
    parti: tuple[int, bytes] | None = None,
) -> PhysicalNand:
    """Write main-area content into an erased flash and interleave 64 OOB bytes per page."""
    main = bytearray(b"\xff" * (BLOCKS * BLOCK_MAIN_BYTES))
    main[APPLICATION_OFFSET : APPLICATION_OFFSET + len(image)] = image
    for index, copy in enumerate(vbm):
        offset = (BLOCKS - len(vbm) + index) * BLOCK_MAIN_BYTES
        main[offset : offset + len(copy)] = copy
    if parti is not None:
        offset, table = parti
        main[offset : offset + len(table)] = table
    pages = (
        bytes(main[offset : offset + PAGE_MAIN_BYTES]) + b"\xff" * 64
        for offset in range(0, len(main), PAGE_MAIN_BYTES)
    )
    return PhysicalNand(b"".join(pages), PAGE_MAIN_BYTES + 64)


def _agreeing_vbm() -> tuple[bytes, bytes]:
    return (_vbm_copy(VBM_ENTRIES, b"first-copy"), _vbm_copy(VBM_ENTRIES, b"secondcopy"))


class PartitionDiscoveryTests(unittest.TestCase):
    """Find the stock application image by searching, not by fixed table offsets."""

    def test_vbm_copies_at_the_end_select_the_signed_image_only(self) -> None:
        """The erased rest of the partition is not passed on as stock image data."""
        image = _signed_image(b"stock application" * 64)

        found = STOCK_IMAGE.find_application_image(_backup(image, vbm=_agreeing_vbm()))

        self.assertEqual(found.contents, image)
        self.assertEqual(found.nand_offset, APPLICATION_OFFSET)
        self.assertEqual(found.partition_table, "VBM")

    def test_parti_table_anywhere_selects_the_same_image(self) -> None:
        """A PartI table across a page boundary, away from any block start, is found."""
        image = _signed_image(b"stock application" * 64)

        found = STOCK_IMAGE.find_application_image(
            _backup(image, parti=(0x7FC, _parti_table(PARTI_ENTRIES)))
        )

        self.assertEqual(found.contents, image)
        self.assertEqual(found.nand_offset, APPLICATION_OFFSET)
        self.assertEqual(found.partition_table, "PartI")

    def test_disagreeing_vbm_copies_are_refused(self) -> None:
        """Two readable but different copies leave the layout ambiguous."""
        changed = (*VBM_ENTRIES[:1], (0x00000003, 0x100, 2, 4), *VBM_ENTRIES[2:])
        vbm = (_vbm_copy(VBM_ENTRIES, b"first-copy"), _vbm_copy(changed, b"secondcopy"))

        with self.assertRaisesRegex(ValueError, "VBM.*disagree"):
            STOCK_IMAGE.find_application_image(_backup(_signed_image(b"image"), vbm=vbm))

    def test_missing_tables_or_image_are_refused(self) -> None:
        """No table, one VBM copy, no application entry or no signed image stops extraction."""
        image = _signed_image(b"image")
        without_application = tuple(
            (0x00000004, *entry[1:]) if entry[0] == 3 else entry for entry in PARTI_ENTRIES
        )
        oversized = bytearray(image)
        oversized[0x30:0x34] = struct.pack("<I", 4 * BLOCK_MAIN_BYTES)
        cases = {
            "no table": (_backup(image), "no VBM or PartI partition table"),
            "one VBM copy": (
                _backup(image, vbm=_agreeing_vbm()[:1]),
                "expected two VBM partition-table copies, found 1",
            ),
            "no application entry": (
                _backup(image, parti=(0x1000, _parti_table(without_application))),
                "no stock application image partition",
            ),
            "no signed image": (
                _backup(b"\x00" * 0x400, vbm=_agreeing_vbm()),
                "no DHTB header",
            ),
            "image larger than its partition": (
                _backup(bytes(oversized), vbm=_agreeing_vbm()),
                "does not fit",
            ),
        }
        for name, (nand, message) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(ValueError, message):
                STOCK_IMAGE.find_application_image(nand)


# Keymap index to stock code: column 0 has rows 0 and 1 and an empty row 2;
# column 1 has rows 0 to 2, row 2 holding a code without a phone key.
SPARSE_KEYMAP = {0: 0x2A, 1: 0x31, 8: 0x0D, 9: 0x05, 10: 0x24}
SPARSE_MATRIX = [
    {"row": 0, "column": 0, "code": "0x2a", "key": "FPLINUX_KEY_STAR"},
    {"row": 0, "column": 1, "code": "0x0d", "key": "FPLINUX_KEY_OK"},
    {"row": 1, "column": 0, "code": "0x31", "key": "FPLINUX_KEY_1"},
    {"row": 1, "column": 1, "code": "0x05", "key": "FPLINUX_KEY_DOWN"},
    {"row": 2, "column": 1, "code": "0x24", "key": None},
]


def _keymap(codes: dict[int, int]) -> bytes:
    """Encode 64 little-endian key codes by keymap index; other entries are empty."""
    return b"".join(struct.pack("<H", codes.get(index, 0xFFFF)) for index in range(64))


class KeypadTests(unittest.TestCase):
    """Turn the stock keymap into the matrix, boot key and EIC9 candidate."""

    def test_matrix_positions_follow_the_column_major_keymap_with_a_hole(self) -> None:
        """Entry column * 8 + row is MATRIX_KEY(row, column); an empty entry is no key."""
        report = STOCK_IMAGE.keypad_report(_keymap(SPARSE_KEYMAP))

        self.assertEqual(report["matrix"], SPARSE_MATRIX)
        self.assertEqual((report["rows"], report["columns"]), (3, 2))
        self.assertEqual(report["boot_key"], {"code": "0x2a", "key": "FPLINUX_KEY_STAR"})

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
        one_missing = STOCK_IMAGE.keypad_report(_keymap(keys_without_8))
        two_missing = STOCK_IMAGE.keypad_report(_keymap(keys_without_8 | {18: 0xFFFF}))

        eight = {"code": "0x38", "key": "FPLINUX_KEY_8"}
        self.assertEqual(one_missing["eic9_candidate"], eight)
        self.assertEqual(one_missing["missing_keys"], [eight])
        self.assertIsNone(two_missing["eic9_candidate"])
        self.assertEqual(
            two_missing["missing_keys"],
            [{"code": "0x0d", "key": "FPLINUX_KEY_OK"}, eight],
        )


class NandConfigTests(unittest.TestCase):
    """Decode the stock NAND configuration table with its documented record layout."""

    def test_records_are_decoded_up_to_the_terminating_record(self) -> None:
        """Bytes after the terminator are not read as further chips."""
        table = NAND_RECORD_21E5 + NAND_RECORD_B1A1 + NAND_TERMINATOR + NAND_RECORD_21E5

        records = STOCK_IMAGE.decode_nand_configs(table)

        self.assertEqual(len(records), 2)
        self.assertEqual(records[0], EXPECTED_21E5)
        self.assertEqual((records[1]["id"], records[1]["block_count"]), ("0xb1a1", 2048))

    def test_table_without_a_terminating_record_is_refused(self) -> None:
        """A truncated table cannot be reported as complete."""
        with self.assertRaisesRegex(ValueError, "no terminating record"):
            STOCK_IMAGE.decode_nand_configs(NAND_RECORD_21E5 + NAND_RECORD_B1A1)


BASE = 0x80100000
DATA = 0x81000000
INIT_ENTRY = "0x80103006"
REPORT_PINMAP = b"".join(
    struct.pack("<II", *pair)
    for pair in (
        (0x402A00B0, 0x00000000),
        (0x402A00B8, 0x00000010),
        (0x402A04B0, 0x00182001),
        (0x402A0100, 0x00000005),
        (0x402A04F0, 0x0008E001),
        (0x402A0504, 0x0008E000),
        (0x40608640, 0x00000001),
        (0xFFFFFFFF, 0xFFFFFFFF),
        (0xFFFFFFFF, 0x0000FFFF),
    )
)
UNPACK_OUTPUT = f"""0x0: DHTB header (size = 0x5000)
0x800: init_table, start = 0x900, end = 0x910
0x808: init_lzdec2
0: src = 0x80100a00, dst = 0x{DATA:08x}, len = 0x400, fn = 0x80100809

ps_addr: 0x{BASE:x}
ps_size: 0x5000
0x81000080 (0x81000000 + 0x80): fat_config
guess: keymap addr = 0x80104900
0x81000100: LCD, id = 0x003025 (0, 0, 1), addr = 0x80102000
0x8100012c: LCD, id = 0x009106 (1, 2, 5), addr = 0x80102400
0x1000: nand configs (size = 0x70)
0x4800: pinmap (end = 0x4840)
0x4c00: init_table, start = 0x4d00, end = 0x4d10
0x81800000: LCD, id = 0x00dead (0, 0, 1), addr = 0x81800100
pinmap: LCD pins = 0x10, 0x10, 0x10, 0x10, 0x10
0x4900: keymap, bootkey = 0x2a (STAR), bl_update keys = 0x31 0x0d (1, CENTER)
"""
INIT_OUTPUT = """LCM_DELAY(10),
LCM_CMD(0xff, 1), 0xa5,
LCM_CMD(0x11, 0),
LCM_DELAY(120),
LCM_CMD(0x36, 1), 0x00,
!!! 0x3040: unknown op 0x1c64
LCM_CMD(0x29, 2), 0x00,0x00,
LCM_END
"""


def _report_image(*, with_fuel_gauge: bool = True) -> bytes:
    """Lay out the stock structures that the report reads beside the tool's output."""
    image = bytearray(_signed_image(bytes(0x5000)))

    def put(offset: int, value: bytes) -> None:
        image[offset : offset + len(value)] = value

    put(0x1000, NAND_RECORD_21E5 + NAND_TERMINATOR)
    # LCM panel: 128x160, interface 1, timing record and operation table.
    put(0x2000, struct.pack("<7I", 128, 160, 1, 0, 2, BASE + 0x2100, BASE + 0x2200))
    put(0x2100, struct.pack("<6I", 50, 55, 110, 20, 70, 35))
    put(0x2200, struct.pack("<I", BASE + 0x3238 + 1))
    # SPI panel: 240x320, interface 0, 24 MHz.
    put(0x2400, struct.pack("<7I", 240, 320, 0, 0, 2, BASE + 0x2500, BASE + 0x2600))
    put(0x2500, struct.pack("<6I", 24000000, 0, 0, 8, 0, 0))
    put(0x2600, struct.pack("<I", BASE + 0x3400 + 1))
    # push {r4, lr}; bl reset; movs r0, #10
    put(0x3000, bytes.fromhex("10b5 28f7a8d8 0a20"))
    # adr r0, name; push {r4, lr}; bl trace; bl 0x3000; movs r0, #0; pop {r4, pc}
    put(0x3238, bytes.fromhex("05a0 10b5 4af488ff fff7defe 0020 10bd"))
    put(0x3250, b"NV3023_Init\0")
    # movs r0, #0xfe; push {r4, lr}: not the decodable prologue
    put(0x329A, bytes.fromhex("fe20 10b5"))
    # push {r4, lr}; bl 0x329a; movs r0, #0; pop {r4, pc}
    put(0x3400, bytes.fromhex("10b5 fff74aff 0020 10bd"))
    if with_fuel_gauge:
        # ldr r2, [pc, #0x338] ... str r1, [r2, #0x34]; ldrd r3, r0, [r2, #0x14]
        put(0x4000, bytes.fromhex("ce4a 00f63320 1521 00eb8000 01eb4000 2a21 b0fbf1f1 5163"))
        put(0x4018, bytes.fromhex("d2e90530"))
        put(0x433C, struct.pack("<I", DATA + 0x300))
    return bytes(image)


def _report_data() -> bytes:
    """Unpacked data with the analog-device table and the fuel-gauge calibration pair."""
    data = bytearray(0x400)
    handler = BASE + 0x3601
    # (device, level, handler): device 1 is the white LED backlight.
    records = (
        (0, 1, handler),
        (1, 31, handler),
        (2, 15, handler),
        (3, 7, handler),
        (4, 1, 0),
        (5, 100, handler),
        (9, 0, 0),
    )
    for index, (identifier, level, function) in enumerate(records):
        offset = 0x200 + index * 24
        data[offset : offset + 24] = struct.pack("<6I", identifier, level, 0, 0, 10, function)
    data[0x314:0x31C] = struct.pack("<2I", 11, 23)
    return bytes(data)


class BoardReportTests(unittest.TestCase):
    """Run extraction against a scripted stand-in for fphelper_t117."""

    def setUp(self) -> None:
        """Install the fake tool as the only host tool of a temporary build."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.host_tools = Path(temporary.name) / "host"
        self.host_tools.mkdir()
        self.tool = self.host_tools / "fphelper_t117"
        shutil.copyfile(FAKE_TOOL, self.tool)
        self.tool.chmod(0o755)

    def _scenario(self, image: bytes, commands: dict[str, dict[str, Any]]) -> None:
        (self.host_tools / "scenario.json").write_text(
            json.dumps({"image_sha256": hashlib.sha256(image).hexdigest(), "commands": commands}),
            encoding="utf-8",
        )

    def _complete_scenario(self, image: bytes) -> None:
        self._scenario(
            image,
            {
                "unpack": {
                    "stdout": UNPACK_OUTPUT,
                    "files": {
                        "pinmap.bin": REPORT_PINMAP.hex(),
                        "keymap.bin": _keymap(SPARSE_KEYMAP).hex(),
                        f"init_{DATA:08x}.bin": _report_data().hex(),
                    },
                },
                f"base 0x{BASE:x} lcd_init_dec {INIT_ENTRY} 1": {"stdout": INIT_OUTPUT},
            },
        )

    def _prepare(self, image: bytes) -> PreparedGroup:
        nand = _backup(image, vbm=_agreeing_vbm())
        group: PreparedGroup = STOCK_IMAGE.prepare_board_maps(nand, host_tools=self.host_tools)
        return group

    def test_report_lists_every_value_with_its_source_and_keeps_the_maps(self) -> None:
        """The maps are the tool's bytes; the report reads the rest from the stock image."""
        image = _report_image()
        self._complete_scenario(image)

        group = self._prepare(image)

        self.assertEqual(
            group.prepared, {"pinmap.bin": REPORT_PINMAP, "keymap.bin": _keymap(SPARSE_KEYMAP)}
        )
        self.assertEqual(group.originals, {"stock-image.bin": image})
        report = json.loads(group.reports["board-report.json"])
        self.assertEqual(
            report["stock_image"],
            {
                "partition_table": "VBM",
                "nand_offset": "0x40000",
                "size": 0x5200,
                "sha256": hashlib.sha256(image).hexdigest(),
                "load_address": "0x80100000",
            },
        )
        tool_sha256 = hashlib.sha256(self.tool.read_bytes()).hexdigest()
        self.assertEqual(report["tool"], {"name": "fphelper_t117", "sha256": tool_sha256})
        self.assertEqual(report["keypad"]["matrix"], SPARSE_MATRIX)
        self.assertEqual(report["keypad"]["boot_key"], {"code": "0x2a", "key": "FPLINUX_KEY_STAR"})
        self.assertEqual(
            report["lcd_candidates"],
            [
                {
                    "id": "0x3025",
                    "controller": "NV3023",
                    "width": 128,
                    "height": 160,
                    "interface": "LCM",
                    "dbi_timing_ns": [50, 55, 110, 20, 70, 35],
                    "init": {
                        "entry": INIT_ENTRY,
                        "steps": [
                            {"delay_ms": 10},
                            {"command": "0xff", "data": "a5"},
                            {"command": "0x11", "data": ""},
                            {"delay_ms": 120},
                            {"command": "0x36", "data": "00"},
                            {"command": "0x29", "data": "0000"},
                        ],
                        "decoder_stop": "0x3040: unknown op 0x1c64",
                    },
                    "source": "stock firmware address 0x81000100",
                },
                {
                    "id": "0x9106",
                    "controller": None,
                    "width": 240,
                    "height": 320,
                    "interface": "SPI",
                    "spi_clock_hz": 24000000,
                    "init": {
                        "unresolved": "the init function does not start with push and a call"
                    },
                    "source": "stock firmware address 0x8100012c",
                },
            ],
        )
        self.assertEqual(
            report["pads"],
            {
                "display": {
                    "0x402a00b0": "0x00000000",
                    "0x402a00b8": "0x00000010",
                    "0x402a04b0": "0x00182001",
                },
                "display_data": {},
                "audio": {"0x402a04f0": "0x0008e001", "0x402a0504": "0x0008e000"},
            },
        )
        self.assertEqual(report["wled_level"], 31)
        self.assertEqual(report["fgu_current_calibration"], [11, 23])
        self.assertEqual(report["nand_configs"], [EXPECTED_21E5])
        self.assertEqual(report["unresolved"], [])
        self.assertEqual(
            {
                name: report["sources"][name]
                for name in ("pinmap", "keymap", "wled_level", "fgu_current_calibration")
            },
            {
                "pinmap": "fphelper_t117 unpack, pinmap.bin from stock firmware address "
                "0x80104800",
                "keymap": "fphelper_t117 unpack, keymap.bin from stock firmware address "
                "0x80104900",
                "wled_level": "stock firmware address 0x8100021c",
                "fgu_current_calibration": "stock firmware address 0x81000314",
            },
        )
        self.assertEqual(
            [item["item"] for item in report["never_extracted"]],
            [
                "keypad light current code",
                "vibration motor presence",
                "fitted panel",
                "CM4 firmware revision",
            ],
        )

    def test_value_without_its_stock_structure_is_unresolved_not_guessed(self) -> None:
        """A missing fuel-gauge conversion leaves no calibration value in the report."""
        image = _report_image(with_fuel_gauge=False)
        self._complete_scenario(image)

        report = json.loads(self._prepare(image).reports["board-report.json"])

        self.assertIsNone(report["fgu_current_calibration"])
        self.assertNotIn("fgu_current_calibration", report["sources"])
        self.assertEqual(
            report["unresolved"],
            [{"item": "fgu_current_calibration", "reason": "no unique fuel-gauge conversion"}],
        )

    def test_missing_tool_failed_tool_or_missing_maps_stop_extraction(self) -> None:
        """No board-map files are produced without a successful tool run that found them."""
        image = _report_image()
        cases: dict[str, tuple[dict[str, dict[str, Any]], str]] = {
            "tool failure": (
                {"unpack": {"stderr": "loadfile failed\n", "exit_status": 1}},
                "fphelper_t117 unpack failed: loadfile failed",
            ),
            "no init table": (
                {"unpack": {"stdout": "0x0: DHTB header (size = 0x5000)\n"}},
                "found no init table",
            ),
            "no maps reported": (
                {"unpack": {"stdout": UNPACK_OUTPUT.replace(": pinmap", ": other")}},
                "board maps not found in the stock image",
            ),
            "maps reported but not written": (
                {"unpack": {"stdout": UNPACK_OUTPUT}},
                "board maps not found in the stock image",
            ),
        }
        for name, (commands, message) in cases.items():
            self._scenario(image, commands)
            with self.subTest(name), self.assertRaisesRegex(ValueError, message):
                self._prepare(image)

        self.tool.unlink()
        with self.assertRaisesRegex(ValueError, "host tool fphelper_t117 is missing"):
            self._prepare(image)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Generate stock board reports with a real process running a controlled fake tool."""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any

from tests import ROOT
from tests.fixtures.stock_board_report import (
    EXPECTED_21E5,
    NAND_RECORD_21E5,
    NAND_TERMINATOR,
    SPARSE_KEYMAP,
    SPARSE_MATRIX,
    STOCK_IMAGE,
    agreeing_vbm,
    backup,
    keymap,
    signed_image,
)

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import PreparedGroup

FAKE_TOOL = ROOT / "tests/fixtures/fphelper/fake_fphelper.py"

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
    image = bytearray(signed_image(bytes(0x5000)))

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
                        "keymap.bin": keymap(SPARSE_KEYMAP).hex(),
                        f"init_{DATA:08x}.bin": _report_data().hex(),
                    },
                },
                f"base 0x{BASE:x} lcd_init_dec {INIT_ENTRY} 1": {"stdout": INIT_OUTPUT},
            },
        )

    def _prepare(self, image: bytes) -> PreparedGroup:
        nand = backup(image, vbm=agreeing_vbm())
        group: PreparedGroup = STOCK_IMAGE.prepare_board_maps(nand, host_tools=self.host_tools)
        return group

    def test_report_lists_every_value_with_its_source_and_keeps_the_maps(self) -> None:
        """The maps are the tool's bytes; the report reads the rest from the stock image."""
        image = _report_image()
        self._complete_scenario(image)

        group = self._prepare(image)

        self.assertEqual(
            group.prepared, {"pinmap.bin": REPORT_PINMAP, "keymap.bin": keymap(SPARSE_KEYMAP)}
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
            "maps written but not reported": (
                {
                    "unpack": {
                        "stdout": UNPACK_OUTPUT.replace(": pinmap", ": other"),
                        "files": {
                            "pinmap.bin": REPORT_PINMAP.hex(),
                            "keymap.bin": keymap(SPARSE_KEYMAP).hex(),
                        },
                    }
                },
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

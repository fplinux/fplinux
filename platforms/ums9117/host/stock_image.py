# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Extract UMS9117 board maps and a board report from a phone's stock firmware.

The input is the phone's own complete NAND backup. The stock application image
is found through the phone's partition table and passed to the pinned
fphelper_t117 host tool, which locates and unpacks the firmware tables. The
loader pin map and keymap are the tool's output unchanged. The remaining board
values are read from the unpacked image; each reported value names where it
came from, and a value that cannot be found is reported as unresolved instead
of being guessed. Choices that the stock firmware does not record are left to a
person.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from fplinux_cli.device_data import (
    BLOCK_MAIN_BYTES,
    PhysicalNand,
    PreparedGroup,
    RequiredPartition,
    redundant_vbm_partitions,
)

TOOL_NAME = "fphelper_t117"
IMAGE_NAME = "stock-image.bin"
REPORT_NAME = "board-report.json"
_TOOL_SECONDS = 120

# The stock application image, as the phone's partition tables name it.
APPLICATION_PARTITION = 0x00000003
APPLICATION_ATTRIBUTES = 0x100

_VBM_MAGIC = b"VBM_BOOT"
# A compiled PartI table starts with its entry count and an extent at block 0;
# the second extent starts where the first one ends. Its last entry covers the
# remaining blocks with this count.
_PARTI_CANDIDATE = re.compile(rb"(?s)(?=[\x02-\x64]\x00\x00\x00.{8}\x00{4}(.{4}).{8}\1)")
_PARTI_ENTRY = struct.Struct("<4I")
_PARTI_REMAINDER = 0xFFFFFFFF

# The signed image header; the image follows it and its size is at 0x30.
_DHTB_MAGIC = b"DHTB"
_DHTB_HEADER_BYTES = 0x200

# Stock key codes and the matching phone key codes.
KEY_NAMES = {
    0x01: "FPLINUX_KEY_CALL",
    0x04: "FPLINUX_KEY_UP",
    0x05: "FPLINUX_KEY_DOWN",
    0x06: "FPLINUX_KEY_LEFT",
    0x07: "FPLINUX_KEY_RIGHT",
    0x08: "FPLINUX_KEY_SOFT_LEFT",
    0x09: "FPLINUX_KEY_SOFT_RIGHT",
    0x0D: "FPLINUX_KEY_OK",
    0x23: "FPLINUX_KEY_POUND",
    0x2A: "FPLINUX_KEY_STAR",
    **{0x30 + digit: f"FPLINUX_KEY_{digit}" for digit in range(10)},
}
# Every phone keypad has all of these keys.
_NEEDED_KEYS = frozenset(KEY_NAMES)
_EMPTY_KEY = 0xFFFF
_KEYMAP_COLUMN_STRIDE = 8
_KEYMAP_MAX_BYTES = 128

# Pin-map registers of the display control pads, the parallel display data
# pads and the audio pads: (first register, register count) per window.
_PAD_WINDOWS = {
    "display": ((0x402A00B0, 7), (0x402A04B0, 7)),
    "display_data": ((0x402A01F8, 10), (0x402A05F8, 10)),
    "audio": ((0x402A04F0, 8),),
}
_PINMAP_WINDOWS = ((0x402A0000, 0x1000), (0x40608000, 0x1000))
_PINMAP_END = 0xFFFFFFFF

# The stock analog-device table: 24-byte records {id, level, 0, 0, 10, handler}
# for devices 0-5 and 9. Device 1 is the white LED backlight driver.
_ANALOG_DEVICE_IDS = (0, 1, 2, 3, 4, 5, 9)
_ANALOG_RECORD_BYTES = 24
_WLED_DEVICE = 1
_ANALOG_TABLE = re.compile(
    b"(?s)"
    + b"".join(
        re.escape(struct.pack("<I", identifier))
        + b".{4}"
        + re.escape(bytes(8) + struct.pack("<I", 10))
        + b".{4}"
        for identifier in _ANALOG_DEVICE_IDS
    )
)

# The stock fuel-gauge current conversion: ldr r2, =calibration; addw r0, r0,
# #0xa33; movs r1, #0x15; add.w r0, r0, r0, lsl #2; add.w r0, r1, r0, lsl #1;
# movs r1, #0x2a; udiv r1, r0, r1; str r1, [r2, #x]; ldrd r3, r0, [r2, #offset].
# The loaded pair is the real and reference calibration resistance.
_FGU_CODE = re.compile(
    rb"(?s)(.)\x4a\x00\xf6\x33\x20\x15\x21\x00\xeb\x80\x00\x01\xeb\x40\x00"
    rb"\x2a\x21\xb0\xfb\xf1\xf1..\xd2\xe9(.)\x30"
)

# One NAND configuration record: a 16-bit chip ID widened to 64 bits, then the
# chip geometry, bad-block, ECC and timing fields. A record with a zero ID and
# a zero block count ends the table.
_NAND_CONFIG = struct.Struct("<QIIIHHHHBBBBBbBBBBbbbBBBHHHHI")
_NAND_CONFIG_FIELDS = (
    "id",
    "block_count",
    "plane_count",
    "reserved_block_count",
    "sector_bytes",
    "spare_bytes_per_sector",
    "sectors_per_page",
    "pages_per_block",
    "write_count_per_page",
    "bad_page_index",
    "bad_sector_index",
    "bad_marker_position",
    "bad_marker_length",
    "safe_data_position",
    "safe_data_length",
    "main_ecc_position",
    "main_ecc_bits",
    "main_ecc_risk_bits",
    "spare_ecc_position",
    "spare_ecc_bits",
    "spare_ecc_risk_bits",
    "bus_width",
    "address_cycles",
    "advance",
    "tr_time",
    "tw_time",
    "te_time",
    "tf_time",
)

# Values that the stock image does not record, with what decides each one.
NEVER_EXTRACTED = (
    {
        "item": "keypad light current code",
        "decided_by": "the stock firmware sets it at run time; measure it on the phone",
    },
    {
        "item": "vibration motor presence",
        "decided_by": "the stock tables list a vibrator even on phones without a motor; "
        "inspect the phone",
    },
    {
        "item": "fitted panel",
        "decided_by": "the stock firmware probes the panel at run time; choose one LCD candidate",
    },
    {
        "item": "CM4 firmware revision",
        "decided_by": "a review of the phone's CM4 image",
    },
)

_SCAN_TABLE = re.compile(r"^0x[0-9a-f]+: init_table\b", re.MULTILINE)
_SCAN_BASE = re.compile(r"^ps_addr: (0x[0-9a-f]+)$", re.MULTILINE)
_SCAN_LCD_LIST = re.compile(
    r"^(0x[0-9a-f]+) \(0x[0-9a-f]+ \+ 0x[0-9a-f]+\): fat_config$", re.MULTILINE
)
_SCAN_LCD = re.compile(
    r"^(0x[0-9a-f]+): LCD, id = (0x[0-9a-f]+) \(\d+, \d+, \d+\), addr = (0x[0-9a-f]+)$",
    re.MULTILINE,
)
_SCAN_PINMAP = re.compile(r"^(0x[0-9a-f]+): pinmap \(end = 0x[0-9a-f]+\)$", re.MULTILINE)
_SCAN_KEYMAP = re.compile(r"^(0x[0-9a-f]+): keymap\b", re.MULTILINE)
_SCAN_NAND = re.compile(r"^(0x[0-9a-f]+): nand configs \(size = 0x[0-9a-f]+\)$", re.MULTILINE)
_UNPACKED_SEGMENT = re.compile(r"init_([0-9a-f]{8})\.bin")

_INIT_DELAY = re.compile(r"LCM_DELAY\((\d+)\),")
_INIT_COMMAND = re.compile(r"LCM_CMD\((0x[0-9a-f]{2}), (\d+)\),((?: (?:0x[0-9a-f]{2},)+)?)")
_INIT_DATA = re.compile(r"LCM_DATA\((\d+)\), ((?:0x[0-9a-f]{2},)+)")
_WRAPPER_HALFWORDS = 32
_NAME_BYTES = 64


@dataclass(frozen=True)
class StockImage:
    """The signed stock application image read from one NAND backup."""

    contents: bytes
    nand_offset: int
    partition_table: str


def find_application_image(nand: PhysicalNand) -> StockImage:
    """Locate the stock application image through the phone's own partition table.

    Two agreeing VBM copies at block starts take precedence; without VBM copies,
    one compiled PartI table found anywhere in the main area is used.
    """
    copies = _vbm_copy_offsets(nand)
    if len(copies) == 2:
        extents = redundant_vbm_partitions(
            nand,
            (copies[0], copies[1]),
            {
                APPLICATION_PARTITION: RequiredPartition(
                    "stock application image", APPLICATION_ATTRIBUTES
                )
            },
        )
        extent = extents[APPLICATION_PARTITION]
        table = "VBM"
    elif copies:
        message = f"expected two VBM partition-table copies, found {len(copies)}"
        raise ValueError(message)
    else:
        extent = _parti_application_extent(nand)
        table = "PartI"
    partition = nand.partition_bytes(extent)
    return StockImage(_signed_image(partition), extent[0], table)


def _vbm_copy_offsets(nand: PhysicalNand) -> list[int]:
    """Return every block whose first page starts with the VBM table magic."""
    offsets = []
    for block in range(nand.block_count):
        offset = block * BLOCK_MAIN_BYTES
        if nand.main_bytes(offset, len(_VBM_MAGIC)) == _VBM_MAGIC:
            offsets.append(offset)
    return offsets


def _parti_application_extent(nand: PhysicalNand) -> tuple[int, int]:
    """Admit the application extent from one consistent compiled PartI table."""
    main = nand.main_bytes(0, nand.block_count * BLOCK_MAIN_BYTES)
    tables = set()
    for candidate in _PARTI_CANDIDATE.finditer(main):
        if candidate.start() % 4 == 0:
            table = _parti_table(main, candidate.start(), nand.block_count)
            if table is not None:
                tables.add(table)
    if not tables:
        message = "no VBM or PartI partition table found"
        raise ValueError(message)
    if len(tables) > 1:
        message = "the NAND holds PartI partition tables that disagree"
        raise ValueError(message)
    (entries,) = tables
    for identifier, attributes, start_block, block_count in entries[:-1]:
        if identifier == APPLICATION_PARTITION:
            if attributes != APPLICATION_ATTRIBUTES:
                message = (
                    f"PartI stock application image has attributes {attributes:#x}; "
                    f"expected {APPLICATION_ATTRIBUTES:#x}"
                )
                raise ValueError(message)
            return start_block * BLOCK_MAIN_BYTES, block_count * BLOCK_MAIN_BYTES
    message = (
        f"PartI table has no stock application image partition ({APPLICATION_PARTITION:#010x})"
    )
    raise ValueError(message)


def _parti_table(
    main: bytes, position: int, block_count: int
) -> tuple[tuple[int, int, int, int], ...] | None:
    """Decode a contiguous PartI table at one position, or None when it is not one."""
    count = struct.unpack_from("<I", main, position)[0]
    end = position + 4 + count * _PARTI_ENTRY.size
    if end > len(main):
        return None
    entries = tuple(
        _PARTI_ENTRY.unpack_from(main, position + 4 + index * _PARTI_ENTRY.size)
        for index in range(count)
    )
    expected_start = 0
    for _identifier, _attributes, start_block, blocks in entries[:-1]:
        if start_block != expected_start or blocks in (0, _PARTI_REMAINDER):
            return None
        expected_start += blocks
    _identifier, _attributes, remainder_start, remainder_blocks = entries[-1]
    if (
        remainder_blocks != _PARTI_REMAINDER
        or remainder_start != expected_start
        or expected_start >= block_count
        or len({entry[0] for entry in entries}) != count
    ):
        return None
    return entries


def _signed_image(partition: bytes) -> bytes:
    """Return the header and signed image, without the unused rest of the partition."""
    if (
        len(partition) < _DHTB_HEADER_BYTES
        or partition[:4] != _DHTB_MAGIC
        or struct.unpack_from("<I", partition, 4)[0] != 1
    ):
        message = "stock application image has no DHTB header"
        raise ValueError(message)
    size = struct.unpack_from("<I", partition, 0x30)[0]
    end = _DHTB_HEADER_BYTES + size
    if not size or end > len(partition):
        message = (
            f"stock application image size {size:#x} does not fit its "
            f"{len(partition):#x}-byte partition"
        )
        raise ValueError(message)
    return partition[:end]


def keypad_report(keymap: bytes) -> dict[str, Any]:
    """Describe the keypad matrix, boot key and EIC9 candidate recorded by a keymap.

    Entry ``column * 8 + row`` holds the stock code at that matrix position. When
    exactly one key that every phone keypad needs is absent, that key is the EIC9
    candidate; the stock keymap records no other external key.
    """
    if not keymap or len(keymap) % 2 or len(keymap) > _KEYMAP_MAX_BYTES:
        message = f"keymap.bin has {len(keymap)} bytes; expected an even size up to 128"
        raise ValueError(message)
    codes = struct.unpack(f"<{len(keymap) // 2}H", keymap)
    matrix = []
    for index, code in enumerate(codes):
        if code != _EMPTY_KEY:
            row = index % _KEYMAP_COLUMN_STRIDE
            column = index // _KEYMAP_COLUMN_STRIDE
            matrix.append({"row": row, "column": column, **_key(code)})
    if not matrix:
        message = "keymap.bin assigns no key"
        raise ValueError(message)
    matrix.sort(key=lambda entry: (entry["row"], entry["column"]))
    missing = sorted(_NEEDED_KEYS - set(codes))
    return {
        "rows": max(entry["row"] for entry in matrix) + 1,
        "columns": max(entry["column"] for entry in matrix) + 1,
        "matrix": matrix,
        "boot_key": None if codes[0] == _EMPTY_KEY else _key(codes[0]),
        "missing_keys": [_key(code) for code in missing],
        "eic9_candidate": _key(missing[0]) if len(missing) == 1 else None,
    }


def _key(code: int) -> dict[str, Any]:
    return {"code": f"{code:#04x}", "key": KEY_NAMES.get(code)}


def decode_nand_configs(table: bytes) -> list[dict[str, Any]]:
    """Decode NAND configuration records up to the terminating record."""
    entries: list[dict[str, Any]] = []
    for offset in range(0, len(table) - _NAND_CONFIG.size + 1, _NAND_CONFIG.size):
        values = _NAND_CONFIG.unpack_from(table, offset)[:-1]
        if values[0] == 0 and values[1] == 0:
            return entries
        entry: dict[str, Any] = dict(zip(_NAND_CONFIG_FIELDS, values, strict=True))
        entry["id"] = f"{entry['id']:#06x}"
        entries.append(entry)
    message = "NAND configuration table has no terminating record"
    raise ValueError(message)


def pad_settings(pinmap: bytes) -> dict[str, dict[str, str]]:
    """Return the pin-map values of the display and audio pads, by register."""
    settings = _pinmap_settings(pinmap)
    return {
        name: {
            f"{register:#010x}": f"{settings[register]:#010x}"
            for first, count in windows
            for register in range(first, first + count * 4, 4)
            if register in settings
        }
        for name, windows in _PAD_WINDOWS.items()
    }


def _pinmap_settings(pinmap: bytes) -> dict[int, int]:
    """Read register/value pairs up to the terminator, as the loader accepts them."""
    settings: dict[int, int] = {}
    for position in range(0, len(pinmap) - 7, 8):
        register, value = struct.unpack_from("<II", pinmap, position)
        if register == value == _PINMAP_END:
            return settings
        if not any(start <= register < start + size for start, size in _PINMAP_WINDOWS):
            message = f"pinmap.bin sets register {register:#010x} outside the pin controllers"
            raise ValueError(message)
        settings[register] = value
    message = "pinmap.bin has no terminator"
    raise ValueError(message)


class _StockMemory:
    """Stock firmware addresses backed by the image and its unpacked data segments."""

    def __init__(self, image: bytes, base: int, segments: dict[int, bytes]) -> None:
        self.image = image
        self.base = base
        self.segments = segments

    def read(self, address: int, size: int) -> bytes:
        for start, contents in self.segments.items():
            if start <= address and address + size <= start + len(contents):
                return contents[address - start : address - start + size]
        offset = address - self.base
        if offset >= 0 and offset + size <= len(self.image):
            return self.image[offset : offset + size]
        message = f"stock firmware address {address:#010x} is outside the unpacked image"
        raise ValueError(message)

    def words(self, address: int, count: int) -> tuple[int, ...]:
        return struct.unpack(f"<{count}I", self.read(address, count * 4))

    def halfword(self, address: int) -> int:
        value: int = struct.unpack("<H", self.read(address, 2))[0]
        return value


def prepare_board_maps(nand: PhysicalNand, *, host_tools: Path) -> PreparedGroup:
    """Extract the loader pin map and keymap and write the board report.

    ``host_tools`` is the directory holding the target's built host tools. The
    stock image is kept as the group's original; the report is for review and
    is not a build input.
    """
    tool = host_tools / TOOL_NAME
    if tool.is_symlink() or not tool.is_file():
        message = f"host tool {TOOL_NAME} is missing from the current build; rebuild the target"
        raise ValueError(message)
    image = find_application_image(nand)
    with tempfile.TemporaryDirectory(prefix="fplinux-stock-image-") as temporary:
        work = Path(temporary)
        (work / IMAGE_NAME).write_bytes(image.contents)
        scan = _main_scan(_run_tool(tool, work, "unpack"))
        pinmap = _unpacked_file(work, "pinmap.bin")
        keymap = _unpacked_file(work, "keymap.bin")
        memory = _StockMemory(image.contents, scan.base, _unpacked_segments(work))
        lcd_candidates = [
            _lcd_candidate(memory, tool, work, entry_address=entry, identifier=lcd, spec=spec)
            for entry, lcd, spec in scan.lcds
        ]
    report = _board_report(
        image,
        tool=tool,
        scan=scan,
        memory=memory,
        pinmap=pinmap,
        keymap=keymap,
        lcd_candidates=lcd_candidates,
    )
    return PreparedGroup(
        originals={IMAGE_NAME: image.contents},
        prepared={"pinmap.bin": pinmap, "keymap.bin": keymap},
        reports={REPORT_NAME: (json.dumps(report, indent=2) + "\n").encode("ascii")},
    )


def _run_tool(tool: Path, work: Path, *arguments: str) -> str:
    command = " ".join(arguments)
    try:
        result = subprocess.run(
            [str(tool), IMAGE_NAME, *arguments],
            cwd=work,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=_TOOL_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        message = f"{TOOL_NAME} {command} did not finish within {_TOOL_SECONDS} seconds"
        raise ValueError(message) from error
    except OSError as error:
        message = f"{TOOL_NAME} cannot be run: {error}"
        raise ValueError(message) from error
    if result.returncode:
        detail = result.stderr.strip() or f"exit status {result.returncode}"
        message = f"{TOOL_NAME} {command} failed: {detail}"
        raise ValueError(message)
    return result.stdout


@dataclass(frozen=True)
class _Scan:
    """Locations that fphelper_t117 reported for the main stock image."""

    base: int
    lcd_list: int | None
    lcds: tuple[tuple[int, int, int], ...]
    pinmap_offset: int
    keymap_offset: int
    nand_offset: int | None


def _main_scan(output: str) -> _Scan:
    """Parse the tool's report, keeping LCD entries of the first (main) init table."""
    tables = [match.start() for match in _SCAN_TABLE.finditer(output)]
    if not tables:
        message = f"{TOOL_NAME} found no init table in the stock image"
        raise ValueError(message)
    main = output[tables[0] : tables[1] if len(tables) > 1 else len(output)]
    if "!!! lzdec failed" in main:
        message = f"{TOOL_NAME} could not unpack the stock image data"
        raise ValueError(message)
    base = _SCAN_BASE.search(main)
    pinmap = _SCAN_PINMAP.search(output)
    keymap = _SCAN_KEYMAP.search(output)
    if base is None:
        message = f"{TOOL_NAME} found no load address for the stock image"
        raise ValueError(message)
    if pinmap is None or keymap is None:
        message = "board maps not found in the stock image"
        raise ValueError(message)
    lcd_list = _SCAN_LCD_LIST.search(main)
    nand = _SCAN_NAND.search(output)
    return _Scan(
        base=int(base.group(1), 16),
        lcd_list=None if lcd_list is None else int(lcd_list.group(1), 16),
        lcds=tuple(
            (int(entry, 16), int(identifier, 16), int(spec, 16))
            for entry, identifier, spec in _SCAN_LCD.findall(main)
        ),
        pinmap_offset=int(pinmap.group(1), 16),
        keymap_offset=int(keymap.group(1), 16),
        nand_offset=None if nand is None else int(nand.group(1), 16),
    )


def _unpacked_file(work: Path, name: str) -> bytes:
    path = work / name
    if path.is_symlink() or not path.is_file():
        message = "board maps not found in the stock image"
        raise ValueError(message)
    return path.read_bytes()


def _unpacked_segments(work: Path) -> dict[int, bytes]:
    """Read the data segments that the main init table unpacks, by load address."""
    segments = {}
    for path in sorted(work.iterdir()):
        match = _UNPACKED_SEGMENT.fullmatch(path.name)
        if match is not None and path.is_file() and not path.is_symlink():
            segments[int(match.group(1), 16)] = path.read_bytes()
    return segments


def _lcd_candidate(  # noqa: PLR0913 -- the listed entry and its tool context stay explicit.
    memory: _StockMemory,
    tool: Path,
    work: Path,
    *,
    entry_address: int,
    identifier: int,
    spec: int,
) -> dict[str, Any]:
    """Describe one panel of the stock LCD list from its panel structure.

    The structure holds width, height, interface (0 SPI, 1 LCM), two fixed words,
    the timing record and the operation table; operation 0 initializes the panel.
    """
    width, height, interface, _, _, timing_address, operations = memory.words(spec, 7)
    if interface not in (0, 1) or not width or not height:
        message = f"LCD {identifier:#06x} panel structure at {spec:#010x} is not recognized"
        raise ValueError(message)
    timing = memory.words(timing_address, 6)
    candidate: dict[str, Any] = {
        "id": f"{identifier:#06x}",
        "controller": None,
        "width": width,
        "height": height,
        "interface": "LCM" if interface else "SPI",
    }
    if interface:
        candidate["dbi_timing_ns"] = list(timing)
    else:
        candidate["spi_clock_hz"] = timing[0]
    name, init_function = _init_wrapper(memory, memory.words(operations, 1)[0] & ~1)
    candidate["controller"] = name
    candidate["init"] = _init_sequence(memory, tool, work, init_function)
    candidate["source"] = f"stock firmware address {entry_address:#010x}"
    return candidate


def _init_wrapper(memory: _StockMemory, address: int) -> tuple[str | None, int | None]:
    """Return the controller name and real init function called by an init wrapper.

    The wrapper may pass a trace string in r0 to its first call; its last call
    before returning is the panel's init function.
    """
    name_address = None
    calls: list[int] = []
    position = address
    for _ in range(_WRAPPER_HALFWORDS):
        halfword = memory.halfword(position)
        if halfword & 0xFF00 == 0xBD00:  # pop {..., pc}
            break
        if halfword >> 11 in (0b11101, 0b11110, 0b11111):
            target = _branch_link_target(position, halfword, memory.halfword(position + 2))
            if target is not None:
                calls.append(target)
            position += 4
            continue
        if halfword & 0xFF00 == 0xA000 and not calls:  # adr r0, label
            name_address = ((position + 4) & ~3) + (halfword & 0xFF) * 4
        position += 2
    else:
        return None, None
    name = None if name_address is None else _controller_name(memory, name_address)
    return name, calls[-1] if calls else None


def _branch_link_target(address: int, first: int, second: int) -> int | None:
    """Return the target of a Thumb-2 BL at ``address``, or None for another instruction."""
    if first & 0xF800 != 0xF000 or second & 0xD000 != 0xD000:
        return None
    sign = (first >> 10) & 1
    i1 = 1 ^ ((second >> 13) & 1) ^ sign
    i2 = 1 ^ ((second >> 11) & 1) ^ sign
    offset = (
        (sign << 24) | (i1 << 23) | (i2 << 22) | ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
    )
    if sign:
        offset -= 1 << 25
    return address + 4 + offset


def _controller_name(memory: _StockMemory, address: int) -> str | None:
    """Reduce a trace string such as ``LCD_ST7789P3_SL_BOE :%s`` or ``NV3023_Init``."""
    try:
        text = memory.read(address, _NAME_BYTES)
    except ValueError:
        return None
    raw, terminator, _ = text.partition(b"\0")
    if not terminator or not raw or not all(0x20 <= byte < 0x7F for byte in raw):
        return None
    word = re.split(r"[ :]", raw.decode("ascii"), maxsplit=1)[0]
    return word.removeprefix("LCD_").removesuffix("_Init") or None


def _init_sequence(
    memory: _StockMemory, tool: Path, work: Path, function: int | None
) -> dict[str, Any]:
    """Decode the panel commands of an init function that starts with push and one call.

    The decoder starts after that prologue and follows only immediate loads,
    register moves, forward branches and calls; it stops at the first other
    instruction.
    """
    if function is None:
        return {"unresolved": "no call to an init function found in the init wrapper"}
    function &= ~1
    first = memory.halfword(function)
    call = _branch_link_target(
        function + 2, memory.halfword(function + 2), memory.halfword(function + 4)
    )
    if first & 0xFF00 != 0xB500 or call is None:
        return {"unresolved": "the init function does not start with push and a call"}
    entry = function + 6
    output = _run_tool(tool, work, "base", f"{memory.base:#x}", "lcd_init_dec", f"{entry:#x}", "1")
    steps, stop = _parse_init(output)
    if not any("command" in step for step in steps):
        return {"unresolved": "the decoder found no panel command"}
    return {"entry": f"{entry:#010x}", "steps": steps, "decoder_stop": stop}


def _parse_init(output: str) -> tuple[list[dict[str, Any]], str | None]:
    steps: list[dict[str, Any]] = []
    stop = None
    for line in output.splitlines():
        if (delay := _INIT_DELAY.fullmatch(line)) is not None:
            steps.append({"delay_ms": int(delay.group(1))})
        elif (command := _INIT_COMMAND.fullmatch(line)) is not None:
            data = _init_bytes(command.group(3), int(command.group(2)))
            steps.append({"command": command.group(1), "data": data})
        elif (data_only := _INIT_DATA.fullmatch(line)) is not None:
            steps.append({"data": _init_bytes(data_only.group(2), int(data_only.group(1)))})
        elif line.startswith("!!! "):
            stop = line.removeprefix("!!! ")
        elif line != "LCM_END":
            message = f"unexpected {TOOL_NAME} lcd_init_dec output: {line}"
            raise ValueError(message)
    return steps, stop


def _init_bytes(text: str, count: int) -> str:
    values = bytes(int(value, 16) for value in text.replace(" ", "").split(",") if value)
    if len(values) != count:
        message = f"{TOOL_NAME} lcd_init_dec printed {len(values)} data bytes for {count}"
        raise ValueError(message)
    return values.hex()


def _wled_level(memory: _StockMemory) -> tuple[int, int] | None:
    """Return the white LED backlight level and its address from the analog table."""
    found = [
        (start + match.start(), contents)
        for start, contents in memory.segments.items()
        for match in _ANALOG_TABLE.finditer(contents)
        if match.start() % 4 == 0
    ]
    if len(found) != 1:
        return None
    table, _ = found[0]
    address = table + _WLED_DEVICE * _ANALOG_RECORD_BYTES + 4
    return memory.words(address, 1)[0], address


def _fgu_calibration(memory: _StockMemory) -> tuple[list[int], int] | None:
    """Return the real and reference calibration resistance and their address."""
    found = [match for match in _FGU_CODE.finditer(memory.image) if match.start() % 2 == 0]
    if len(found) != 1:
        return None
    (match,) = found
    load = memory.base + match.start()
    literal = ((load + 4) & ~3) + match.group(1)[0] * 4
    address = memory.words(literal, 1)[0] + match.group(2)[0] * 4
    return list(memory.words(address, 2)), address


def _board_report(  # noqa: PLR0913 -- every report input stays explicit.
    image: StockImage,
    *,
    tool: Path,
    scan: _Scan,
    memory: _StockMemory,
    pinmap: bytes,
    keymap: bytes,
    lcd_candidates: list[dict[str, Any]],
) -> dict[str, Any]:
    """Assemble the review report; every value names where it came from."""

    def address(offset: int) -> str:
        return f"stock firmware address {scan.base + offset:#010x}"

    lcd_list = "" if scan.lcd_list is None else f" at stock firmware address {scan.lcd_list:#010x}"
    unresolved = []
    sources = {
        "pinmap": f"{TOOL_NAME} unpack, pinmap.bin from {address(scan.pinmap_offset)}",
        "keymap": f"{TOOL_NAME} unpack, keymap.bin from {address(scan.keymap_offset)}",
        "keypad": "keymap.bin: entry column * 8 + row; entry 0 is the boot key",
        "eic9_candidate": "keymap.bin: the only needed phone key absent from the matrix",
        "pads": "pinmap.bin",
        "lcd_candidates": (
            f"{TOOL_NAME} unpack LCD list{lcd_list}; each candidate's panel structure and "
            f"init wrapper from its source address; init steps from {TOOL_NAME} lcd_init_dec"
        ),
    }
    if not lcd_candidates:
        unresolved.append({"item": "lcd_candidates", "reason": "no LCD list found"})

    wled = _wled_level(memory)
    if wled is None:
        unresolved.append({"item": "wled_level", "reason": "no unique analog-device table"})
    else:
        sources["wled_level"] = f"stock firmware address {wled[1]:#010x}"
    fgu = _fgu_calibration(memory)
    if fgu is None:
        unresolved.append(
            {"item": "fgu_current_calibration", "reason": "no unique fuel-gauge conversion"}
        )
    else:
        sources["fgu_current_calibration"] = f"stock firmware address {fgu[1]:#010x}"
    nand_configs = None
    if scan.nand_offset is None:
        unresolved.append({"item": "nand_configs", "reason": "no NAND configuration table"})
    else:
        nand_configs = decode_nand_configs(image.contents[scan.nand_offset :])
        sources["nand_configs"] = f"{TOOL_NAME} unpack, {address(scan.nand_offset)}"

    return {
        "stock_image": {
            "partition_table": image.partition_table,
            "nand_offset": f"{image.nand_offset:#x}",
            "size": len(image.contents),
            "sha256": hashlib.sha256(image.contents).hexdigest(),
            "load_address": f"{scan.base:#010x}",
        },
        "tool": {"name": TOOL_NAME, "sha256": hashlib.sha256(tool.read_bytes()).hexdigest()},
        "keypad": keypad_report(keymap),
        "lcd_candidates": lcd_candidates,
        "pads": pad_settings(pinmap),
        "wled_level": None if wled is None else wled[0],
        "fgu_current_calibration": None if fgu is None else fgu[0],
        "nand_configs": nand_configs,
        "sources": sources,
        "unresolved": unresolved,
        "never_extracted": list(NEVER_EXTRACTED),
    }

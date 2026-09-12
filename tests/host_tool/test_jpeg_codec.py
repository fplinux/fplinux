# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for UMS9117 JPEG validation and fixed encode configuration."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "platforms/ums9117/kernel"
HARNESS = ROOT / "tests/host_tool/jpeg-codec.c"
STANDARD_TABLES = ROOT / "tests/host_tool/jpeg-codec-standard-tables.c"
COMPAT = ROOT / "tests/host_tool/jpeg-codec-compat"

# Pillow/libjpeg quality=85, subsampling=1 output, converted to natural DCT order.
# Source receipt SHA-256: 25bb3739664e47c1cd8aadf2ff486d477738b962bc84a097ae7bab5d88084a12.
STANDARD_Q85_NATURAL = bytes.fromhex(
    "05 03 03 05 07 0c 0f 12 04 04 04 06 08 11 12 11 "
    "04 04 05 07 0c 11 15 11 04 05 07 09 0f 1a 18 13 "
    "05 07 0b 11 14 21 1f 17 07 0b 11 13 18 1f 22 1c "
    "0f 13 17 1a 1f 24 24 1e 16 1c 1d 1d 22 1e 1f 1e "
    "05 05 07 0e 1e 1e 1e 1e 05 06 08 14 1e 1e 1e 1e "
    "07 08 11 1e 1e 1e 1e 1e 0e 14 1e 1e 1e 1e 1e 1e "
    "1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e "
    "1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e 1e"
)

ZIGZAG_TO_NATURAL = bytes.fromhex(
    "00 01 08 10 09 02 03 0a 11 18 20 19 12 0b 04 05 "
    "0c 13 1a 21 28 30 29 22 1b 14 0d 06 07 0e 15 1c "
    "23 2a 31 38 39 32 2b 24 1d 16 0f 17 1e 25 2c 33 "
    "3a 3b 34 2d 26 1f 27 2e 35 3c 3d 36 2f 37 3e 3f"
)


def _parse_dump(output: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in output.splitlines():
        name, value = line.split(maxsplit=1)
        if name in fields:
            message = f"duplicate harness field: {name}"
            raise ValueError(message)
        fields[name] = value
    return fields


def _parse_segments(header: bytes) -> list[tuple[int, bytes]]:
    if not header.startswith(b"\xff\xd8"):
        message = "encode prefix does not start with SOI"
        raise ValueError(message)
    position = 2
    segments: list[tuple[int, bytes]] = []
    while position < len(header):
        if position + 4 > len(header) or header[position] != 0xFF:
            message = "truncated or unmarked JPEG header segment"
            raise ValueError(message)
        marker = header[position + 1]
        length = int.from_bytes(header[position + 2 : position + 4], "big")
        end = position + 2 + length
        if length < 2 or end > len(header):
            message = "invalid JPEG header segment length"
            raise ValueError(message)
        segments.append((marker, header[position + 4 : end]))
        position = end
    if not segments:
        message = "encode prefix has no JPEG segments"
        raise ValueError(message)
    return segments


def _collect_dqt(segments: list[tuple[int, bytes]]) -> dict[int, bytes]:
    tables: dict[int, bytes] = {}
    for marker, payload in segments:
        if marker != 0xDB:
            continue
        position = 0
        while position < len(payload):
            descriptor = payload[position]
            position += 1
            precision = descriptor >> 4
            size = 64 * (precision + 1)
            end = position + size
            if precision != 0 or end > len(payload):
                message = "encode DQT is not an 8-bit baseline table"
                raise ValueError(message)
            destination = descriptor & 0x0F
            if destination in tables:
                message = f"duplicate DQT destination: {destination}"
                raise ValueError(message)
            tables[destination] = payload[position:end]
            position = end
    return tables


def _collect_dht(
    segments: list[tuple[int, bytes]],
) -> dict[tuple[int, int], tuple[bytes, bytes]]:
    tables: dict[tuple[int, int], tuple[bytes, bytes]] = {}
    for marker, payload in segments:
        if marker != 0xC4:
            continue
        position = 0
        while position < len(payload):
            if position + 17 > len(payload):
                message = "truncated DHT table"
                raise ValueError(message)
            descriptor = payload[position]
            counts = payload[position + 1 : position + 17]
            end = position + 17 + sum(counts)
            if end > len(payload):
                message = "truncated DHT symbols"
                raise ValueError(message)
            key = (descriptor >> 4, descriptor & 0x0F)
            if key in tables:
                message = f"duplicate DHT table: {key}"
                raise ValueError(message)
            tables[key] = (counts, payload[position + 17 : end])
            position = end
    return tables


def _canonical_codes(counts: bytes, symbols: bytes) -> dict[int, tuple[int, int]]:
    result: dict[int, tuple[int, int]] = {}
    position = 0
    code = 0
    for length, count in enumerate(counts, 1):
        for _ in range(count):
            symbol = symbols[position]
            if symbol in result:
                message = f"duplicate Huffman symbol: {symbol:#x}"
                raise ValueError(message)
            result[symbol] = (code, length)
            code += 1
            position += 1
        code <<= 1
    if position != len(symbols):
        message = "DHT count/symbol length mismatch"
        raise ValueError(message)
    return result


class Ums9117JpegCodecHostTests(unittest.TestCase):
    """Exercise production codec objects through controlled external doubles."""

    def test_decode_contract_and_fixed_encode_config(self) -> None:
        """Preserve decoder behavior and validate the observable encode config."""
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "ums9117-jpeg-codec"
            run_process(
                [
                    "cc",
                    "-O2",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{COMPAT}",
                    f"-I{KERNEL}",
                    str(HARNESS),
                    str(STANDARD_TABLES),
                    str(KERNEL / "ums9117-jpeg-codec.c"),
                    "-o",
                    str(executable),
                ],
                name="compile UMS9117 JPEG codec host oracle",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="run UMS9117 JPEG decode and encode-boundary oracle",
                timeout=30,
                check=True,
            )
            dump = run_process(
                [str(executable), "--dump-encode"],
                name="export UMS9117 JPEG encode config",
                timeout=30,
                check=True,
            )

        fields = _parse_dump(dump.stdout)
        self.assertEqual(set(fields), {"meta", "quant", "header", "qbuf", "ac"})
        self.assertEqual(
            tuple(map(int, fields["meta"].split())),
            (1200, 32, 75, 4, 150),
        )

        quant = bytes.fromhex(fields["quant"])
        self.assertEqual(quant, STANDARD_Q85_NATURAL)
        header = bytes.fromhex(fields["header"])
        segments = _parse_segments(header)
        self.assertEqual(segments[-1][0], 0xDA)

        dqt = _collect_dqt(segments)
        self.assertEqual(set(dqt), {0, 1})
        expected_luma = bytes(STANDARD_Q85_NATURAL[index] for index in ZIGZAG_TO_NATURAL)
        expected_chroma = bytes(STANDARD_Q85_NATURAL[64 + index] for index in ZIGZAG_TO_NATURAL)
        self.assertEqual(dqt[0], expected_luma)
        self.assertEqual(dqt[1], expected_chroma)

        sof_segments = [payload for marker, payload in segments if marker == 0xC0]
        self.assertEqual(len(sof_segments), 1)
        sof = sof_segments[0]
        self.assertEqual(len(sof), 15)
        self.assertEqual(sof[0], 8)
        self.assertEqual(int.from_bytes(sof[1:3], "big"), 32)
        self.assertEqual(int.from_bytes(sof[3:5], "big"), 1200)
        self.assertEqual(sof[5], 3)
        self.assertEqual(
            tuple(tuple(sof[6 + index * 3 : 9 + index * 3]) for index in range(3)),
            ((1, 0x21, 0), (2, 0x11, 1), (3, 0x11, 1)),
        )

        dht = _collect_dht(segments)
        self.assertEqual(set(dht), {(0, 0), (1, 0), (0, 1), (1, 1)})
        dc_symbols = set(range(12))
        ac_symbols = {0, 0xF0} | {run * 16 + size for run in range(16) for size in range(1, 11)}
        for key in ((0, 0), (0, 1)):
            self.assertEqual(set(dht[key][1]), dc_symbols)
        for key in ((1, 0), (1, 1)):
            self.assertEqual(set(dht[key][1]), ac_symbols)

        luma_ac = _canonical_codes(*dht[(1, 0)])
        chroma_ac = _canonical_codes(*dht[(1, 1)])
        self.assertEqual(luma_ac[0x01], (0, 2))
        self.assertEqual(luma_ac[0x00], (0xA, 4))
        self.assertEqual(luma_ac[0xF0], (0x7F9, 11))
        self.assertEqual(luma_ac[0x09], (0xFF82, 16))
        self.assertEqual(luma_ac[0x0A], (0xFF83, 16))
        self.assertEqual(chroma_ac[0x00], (0, 2))
        self.assertEqual(chroma_ac[0x01], (1, 2))
        self.assertEqual(chroma_ac[0xF0], (0x3FA, 10))
        self.assertEqual(chroma_ac[0x09], (0x3F6, 10))
        self.assertEqual(chroma_ac[0x0A], (0xFF4, 12))

        dri_segments = [payload for marker, payload in segments if marker == 0xDD]
        self.assertEqual(dri_segments, [b"\x00\x96"])
        sos_segments = [payload for marker, payload in segments if marker == 0xDA]
        self.assertEqual(len(sos_segments), 1)
        sos = sos_segments[0]
        self.assertEqual(len(sos), 10)
        self.assertEqual(sos[0], 3)
        self.assertEqual(
            tuple(tuple(sos[1 + index * 2 : 3 + index * 2]) for index in range(3)),
            ((1, 0x00), (2, 0x11), (3, 0x11)),
        )
        self.assertEqual(sos[-3:], b"\x00\x3f\x00")

        qbuf = tuple(int(word, 16) for word in fields["qbuf"].split())
        self.assertEqual(len(qbuf), 64)
        self.assertEqual(qbuf[0], 0xCCD3CCD3)
        self.assertEqual(qbuf[16] & 0xFFFF, 0xAAB2)
        self.assertEqual(qbuf[13] & 0xFFFF, 0xC315)
        self.assertEqual(qbuf[32], 0x8895CCD3)
        self.assertEqual(qbuf[63], 0x88958895)

        ac = tuple(int(word, 16) for word in fields["ac"].split())
        self.assertEqual(len(ac), 162)
        expected_ac = {
            0: 0x40000000,
            1: 0x80004000,
            8: 0xFD80FF82,
            9: 0xFF40FF83,
            159: 0xFFFEFFFE,
            160: 0,
            161: 0,
        }
        for index, expected in expected_ac.items():
            with self.subTest(ac_index=index):
                self.assertEqual(ac[index], expected)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks of PSF2 loading, glyph selection and ownership."""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from tests import ROOT
from tests.process import run_process


def psf2(
    glyphs: tuple[bytes, ...],
    mappings: tuple[bytes, ...],
    width: int,
    height: int,
    header_extension: bytes = b"",
) -> bytes:
    """Encode test-owned PSF2 bitmaps and Unicode records without interpreting them."""
    header = struct.pack(
        "<8I",
        0x864AB572,
        0,
        32 + len(header_extension),
        1,
        len(glyphs),
        len(glyphs[0]),
        height,
        width,
    )
    return (
        header
        + header_extension
        + b"".join(glyphs)
        + b"".join(mapping + b"\xff" for mapping in mappings)
    )


class FontTests(unittest.TestCase):
    """Observe the shared loader through real files and a separately linked C harness."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    executable: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the linked font loader once for the controlled-file cases."""
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "font"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include/fplinux"),
                str(ROOT / "tests/host_tool/terminal/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                "-Wl,--wrap=fopen",
                "-o",
                str(cls.executable),
            ],
            name="compile font loader harness",
            timeout=30,
            check=True,
        )

    def inspect(self, payload: bytes, *codepoints: str) -> str:
        """Load a temporary PSF2 fixture and return the harness's observable glyph bytes."""
        with tempfile.TemporaryDirectory() as directory:
            font = Path(directory) / "font.psf"
            font.write_bytes(payload)
            result = run_process(
                [str(self.executable), str(font), *codepoints],
                name="inspect PSF2 font fixture",
                timeout=10,
                check=True,
            )
            return result.stdout

    def inspect_default(self, payload: bytes, display_width: int) -> str:
        """Redirect only the installed default file to a controlled real PSF2 payload."""
        with tempfile.TemporaryDirectory() as directory:
            font = Path(directory) / "font.psf"
            font.write_bytes(payload)
            result = run_process(
                [str(self.executable), "--default", str(display_width), str(font)],
                name="load installed default font for a display width",
                timeout=10,
                check=True,
            )
            return result.stdout

    def test_default_font_metrics_match_the_actual_display_width(self) -> None:
        """The installed payload must be 6x12 below 200 pixels wide and 8x16 otherwise."""
        small = psf2((b"\x80" * 12,), (b"A",), 6, 12)
        large = psf2((b"\x80" * 16,), (b"A",), 8, 16)
        for width in (128, 199):
            with self.subTest(width=width):
                self.assertEqual(self.inspect_default(small, width), "6 12\n")
                self.assertEqual(
                    self.inspect_default(large, width),
                    f"default font is 8x16; {width}-pixel display requires 6x12\n",
                )
        for width in (200, 240):
            with self.subTest(width=width):
                self.assertEqual(self.inspect_default(large, width), "8 16\n")
                self.assertEqual(
                    self.inspect_default(small, width),
                    f"default font is 6x12; {width}-pixel display requires 8x16\n",
                )

    def test_default_font_rejects_a_wrong_height_or_unreadable_payload(self) -> None:
        """Matching width alone is insufficient, and malformed files report the default path."""
        wrong_height = psf2((b"\x80" * 16,), (b"A",), 6, 16)
        self.assertEqual(
            self.inspect_default(wrong_height, 128),
            "default font is 6x16; 128-pixel display requires 6x12\n",
        )
        self.assertEqual(
            self.inspect_default(b"not a PSF font", 240),
            "cannot load font /usr/share/fplinux/fonts/default.psf\n",
        )

    def test_unicode_aliases_sequences_and_replacement_keep_bitmap_rows(self) -> None:
        """UTF-8 aliases resolve; sequence members use the replacement glyph instead."""
        font = psf2(
            (b"\x80\x00\x40\x80", b"\x00\x80\x00\x00", b"\xff\x80\x00\x80"),
            ("ЯA🙂".encode(), "éB".encode() + b"\xfeab", "�".encode()),
            9,
            2,
            b"extension",
        )
        self.assertEqual(
            self.inspect(font, "41", "42f", "1f642", "42", "e9", "61", "58", "fffd"),
            "9 2 2 4\n80004080\n80004080\n80004080\n00800000\n00800000\n"
            "ff800080\nff800080\nff800080\n",
        )

    def test_absent_codepoint_without_replacement_returns_no_bitmap(self) -> None:
        """Sparse fonts leave unknown characters blank when U+FFFD is absent."""
        font = psf2((b"\x80",), (b"A",), 1, 1)
        self.assertEqual(self.inspect(font, "41", "42", "fffd"), "1 1 1 1\n80\nmissing\nmissing\n")

    def test_maximum_geometry_and_file_size_are_inclusive(self) -> None:
        """A 16x32 font of exactly 1 MiB loads; one extra file byte is rejected."""
        font = psf2((b"\x80\x01" * 32,), (b"A",), 16, 32, b"\0" * (1024 * 1024 - 98))
        self.assertEqual(len(font), 1024 * 1024)
        self.assertEqual(self.inspect(font), "16 32 2 64\n")
        self.assertEqual(self.inspect(font + b"\0"), "rejected\n")

    def test_unsupported_or_incomplete_psf2_is_rejected(self) -> None:
        """Malformed headers, out-of-range cells and incomplete tables cannot load."""
        valid = psf2((b"\x80",), (b"A",), 1, 1)
        cases = {
            "short header": valid[:31],
            "no Unicode table": struct.pack("<8I", 0x864AB572, 0, 32, 0, 1, 1, 1, 1) + b"\x80",
            "unsupported version": struct.pack("<8I", 0x864AB572, 1, 32, 1, 1, 1, 1, 1)
            + b"\x80A\xff",
            "width exceeds limit": psf2((b"\0" * 3,), (b"A",), 17, 1),
            "height exceeds limit": psf2((b"\0" * 33,), (b"A",), 1, 33),
            "inconsistent bitmap size": psf2((b"\0\0",), (b"A",), 1, 1),
            "missing glyph terminator": valid[:-1],
            "trailing Unicode record": valid + b"B\xff",
            "no single-codepoint mapping": psf2((b"\x80",), (b"\xfeab",), 1, 1),
        }
        for scenario, font in cases.items():
            with self.subTest(scenario=scenario):
                self.assertEqual(self.inspect(font), "rejected\n")

    def test_invalid_utf8_is_rejected_even_inside_sequences(self) -> None:
        """Invalid UTF-8 never creates ambiguous aliases or silently skipped records."""
        for encoding in (
            b"\xc0\x80",
            b"\xe0\x80\x80",
            b"\xed\xa0\x80",
            b"\xf4\x90\x80\x80",
            b"\xe2\x82",
        ):
            for prefix in (b"", b"A\xfe"):
                with self.subTest(encoding=encoding, sequence=bool(prefix)):
                    self.assertEqual(
                        self.inspect(psf2((b"\x80",), (prefix + encoding,), 1, 1)), "rejected\n"
                    )

    def test_missing_file_leaves_font_safe_to_close(self) -> None:
        """A failed file open leaves no loaded font and needs no caller cleanup branch."""
        with tempfile.TemporaryDirectory() as directory:
            result = run_process(
                [str(self.executable), str(Path(directory) / "absent.psf")],
                name="load missing font fixture",
                timeout=10,
                check=True,
            )
            self.assertEqual(result.stdout, "rejected\n")


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks of PSF2 loading, glyph selection and ownership."""

from __future__ import annotations

import os
import struct
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


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


class FontTests:
    """Observe the shared loader through real files and a separately linked C harness."""

    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        """Compile the linked font loader once for the controlled-file cases."""
        with c_build_directory(tmp_path_factory, "font") as directory:
            cls.executable = directory / "font"
            compile_c(
                cls.executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include/fplinux"),
                    str(ROOT / "tests/host_tool/terminal/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    "-Wl,--wrap=fopen",
                ],
                name="compile font loader harness",
                check=True,
            )
            yield

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

    @pytest.mark.parametrize("locale", ["C", "C.UTF-8"])
    def test_unicode_cursor_advances_only_for_valid_bounded_scalars(self, locale: str) -> None:
        """Decoding preserves invalid input and does not depend on the process locale."""
        environment = os.environ.copy()
        environment["LC_ALL"] = locale
        result = run_process(
            [str(self.executable), "--unicode"],
            name="check bounded UTF-8 cursor and scalar results",
            env=environment,
            timeout=10,
            check=True,
        )
        assert result.stdout == ""

    @pytest.mark.parametrize(
        ("width", "small_expected", "large_expected"),
        [
            pytest.param(
                128,
                "6 12\n",
                "default font is 8x16; 128-pixel display requires 6x12\n",
                id="small-128",
            ),
            pytest.param(
                199,
                "6 12\n",
                "default font is 8x16; 199-pixel display requires 6x12\n",
                id="small-boundary-199",
            ),
            pytest.param(
                200,
                "default font is 6x12; 200-pixel display requires 8x16\n",
                "8 16\n",
                id="large-boundary-200",
            ),
            pytest.param(
                240,
                "default font is 6x12; 240-pixel display requires 8x16\n",
                "8 16\n",
                id="large-240",
            ),
        ],
    )
    def test_default_font_metrics_match_the_actual_display_width(
        self, width: int, small_expected: str, large_expected: str
    ) -> None:
        """The installed payload must be 6x12 below 200 pixels wide and 8x16 otherwise."""
        small = psf2((b"\x80" * 12,), (b"A",), 6, 12)
        large = psf2((b"\x80" * 16,), (b"A",), 8, 16)
        assert self.inspect_default(small, width) == small_expected
        assert self.inspect_default(large, width) == large_expected

    def test_default_font_rejects_a_wrong_height_or_unreadable_payload(self) -> None:
        """Matching width alone is insufficient, and malformed files report the default path."""
        wrong_height = psf2((b"\x80" * 16,), (b"A",), 6, 16)
        assert (self.inspect_default(wrong_height, 128)) == (
            "default font is 6x16; 128-pixel display requires 6x12\n"
        )
        assert (self.inspect_default(b"not a PSF font", 240)) == (
            "cannot load font /usr/share/fplinux/fonts/default.psf\n"
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
        assert (self.inspect(font, "41", "42f", "1f642", "42", "e9", "61", "58", "fffd")) == (
            "9 2 2 4\n80004080\n80004080\n80004080\n00800000\n00800000\n"
            "ff800080\nff800080\nff800080\n"
        )

    def test_absent_codepoint_without_replacement_returns_no_bitmap(self) -> None:
        """Sparse fonts leave unknown characters blank when U+FFFD is absent."""
        font = psf2((b"\x80",), (b"A",), 1, 1)
        assert (self.inspect(font, "41", "42", "fffd")) == ("1 1 1 1\n80\nmissing\nmissing\n")

    @pytest.mark.parametrize(
        ("encoding", "codepoint"),
        [
            pytest.param(b"\x00", "0", id="nul"),
            pytest.param(b"\x7f", "7f", id="last-ascii"),
            pytest.param(b"\xc2\x80", "80", id="first-two-byte"),
            pytest.param(b"\xdf\xbf", "7ff", id="last-two-byte"),
            pytest.param(b"\xe0\xa0\x80", "800", id="first-three-byte"),
            pytest.param(b"\xed\x9f\xbf", "d7ff", id="before-surrogates"),
            pytest.param(b"\xee\x80\x80", "e000", id="after-surrogates"),
            pytest.param(b"\xef\xbf\xbf", "ffff", id="last-three-byte"),
            pytest.param(b"\xf0\x90\x80\x80", "10000", id="first-four-byte"),
            pytest.param(b"\xf4\x8f\xbf\xbf", "10ffff", id="last-scalar"),
        ],
    )
    def test_unicode_scalar_boundaries_resolve_to_their_bitmap(
        self, encoding: bytes, codepoint: str
    ) -> None:
        """Every UTF-8 length accepts its scalar boundaries, including NUL and noncharacters."""
        font = psf2((b"\x80",), (encoding,), 1, 1)
        assert self.inspect(font, codepoint) == "1 1 1 1\n80\n"

    def test_maximum_geometry_and_file_size_are_inclusive(self) -> None:
        """A 16x32 font of exactly 1 MiB loads; one extra file byte is rejected."""
        font = psf2((b"\x80\x01" * 32,), (b"A",), 16, 32, b"\0" * (1024 * 1024 - 98))
        assert (len(font)) == (1024 * 1024)
        assert (self.inspect(font)) == ("16 32 2 64\n")
        assert (self.inspect(font + b"\0")) == ("rejected\n")

    @pytest.mark.parametrize(
        "font",
        [
            pytest.param(psf2((b"\x80",), (b"A",), 1, 1)[:31], id="short-header"),
            pytest.param(
                struct.pack("<8I", 2253043058, 0, 32, 0, 1, 1, 1, 1) + b"\x80",
                id="no-Unicode-table",
            ),
            pytest.param(
                struct.pack("<8I", 2253043058, 1, 32, 1, 1, 1, 1, 1) + b"\x80A\xff",
                id="unsupported-version",
            ),
            pytest.param(psf2((b"\x00" * 3,), (b"A",), 17, 1), id="width-exceeds-limit"),
            pytest.param(psf2((b"\x00" * 33,), (b"A",), 1, 33), id="height-exceeds-limit"),
            pytest.param(psf2((b"\x00\x00",), (b"A",), 1, 1), id="inconsistent-bitmap-size"),
            pytest.param(psf2((b"\x80",), (b"A",), 1, 1)[:-1], id="missing-glyph-terminator"),
            pytest.param(psf2((b"\x80",), (b"A",), 1, 1) + b"B\xff", id="trailing-Unicode-record"),
            pytest.param(psf2((b"\x80",), (b"\xfeab",), 1, 1), id="no-single-codepoint-mapping"),
        ],
    )
    def test_unsupported_or_incomplete_psf2_is_rejected(self, font: bytes) -> None:
        """Malformed headers, out-of-range cells and incomplete tables cannot load."""
        assert self.inspect(font) == "rejected\n"

    @pytest.mark.parametrize(
        "encoding",
        [
            pytest.param(b"\x80", id="stray-continuation"),
            pytest.param(b"\xc0\x80", id="overlong-two-byte"),
            pytest.param(b"\xe0\x80\x80", id="overlong-three-byte"),
            pytest.param(b"\xf0\x80\x80\x80", id="overlong-four-byte"),
            pytest.param(b"\xed\xa0\x80", id="first-surrogate"),
            pytest.param(b"\xed\xbf\xbf", id="last-surrogate"),
            pytest.param(b"\xf4\x90\x80\x80", id="above-last-scalar"),
            pytest.param(b"\xf5\x80\x80\x80", id="invalid-leading-byte"),
            pytest.param(b"\xc2", id="truncated-two-byte"),
            pytest.param(b"\xe2\x82", id="truncated-three-byte"),
            pytest.param(b"\xf0\x90\x80", id="truncated-four-byte"),
            pytest.param(b"\xc2A", id="invalid-two-byte-continuation"),
            pytest.param(b"\xe2\x82A", id="invalid-three-byte-continuation"),
            pytest.param(b"\xf0\x90\x80A", id="invalid-four-byte-continuation"),
        ],
    )
    @pytest.mark.parametrize("prefix", [b"", b"A\xfe"], ids=["bytes-", "bytes-41fe"])
    def test_invalid_utf8_is_rejected_even_inside_sequences(
        self, prefix: bytes, encoding: bytes
    ) -> None:
        """Invalid UTF-8 never creates ambiguous aliases or silently skipped records."""
        assert (self.inspect(psf2((b"\x80",), (prefix + encoding,), 1, 1))) == ("rejected\n")

    def test_missing_file_leaves_font_safe_to_close(self) -> None:
        """A failed file open leaves no loaded font and needs no caller cleanup branch."""
        with tempfile.TemporaryDirectory() as directory:
            result = run_process(
                [str(self.executable), str(Path(directory) / "absent.psf")],
                name="load missing font fixture",
                timeout=10,
                check=True,
            )
            assert (result.stdout) == ("rejected\n")

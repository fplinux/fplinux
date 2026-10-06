# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for source text validation."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import check as source_check
import pytest

if TYPE_CHECKING:
    from pathlib import Path


class SourceTextTests:
    """Validate real file bytes without changing unified-diff syntax."""

    def test_patch_accepts_an_empty_context_row_without_rewriting_it(self, tmp_path: Path) -> None:
        """An empty context line retains its required single-space marker."""
        data = b"--- a/example.txt\n+++ b/example.txt\n@@ -1,2 +1,2 @@\n \n-old\n+new\n"
        root = tmp_path
        path = root / "example.patch"
        path.write_bytes(data)
        with mock.patch.object(source_check, "ROOT", root):
            source_check.check_text([path])
        assert (path.read_bytes()) == (data)

    @pytest.mark.parametrize(
        ("name", "data"),
        [
            ("example.txt", b" \n"),
            ("example.patch", b"  \n"),
            ("example.patch", b"+new \n"),
            ("example.patch", b"+new\t\n"),
            ("example.patch", b" context \n"),
            ("example.patch", b"-old\t\n"),
        ],
        ids=[
            "plain-space",
            "patch-two-spaces",
            "addition-space",
            "addition-tab",
            "context-space",
            "deletion-tab",
        ],
    )
    def test_trailing_whitespace_is_rejected_outside_an_empty_patch_context_row(
        self, tmp_path: Path, name: str, data: bytes
    ) -> None:
        """The diff marker exception must not hide whitespace in actual payloads."""
        root = tmp_path
        path = root / name
        path.write_bytes(data)
        with (
            mock.patch.object(source_check, "ROOT", root),
            pytest.raises(SystemExit, match="trailing whitespace"),
        ):
            source_check.check_text([path])

    @pytest.mark.parametrize("suffix", [".txt", ".patch"], ids=["plain-text", "patch"])
    @pytest.mark.parametrize(
        ("data", "message"),
        [
            (b"\xff\n", "source is not UTF-8"),
            (b"\0\n", "NUL byte in source file"),
            (b"text\r\n", "non-LF line ending"),
            (b"text", "missing final newline"),
        ],
        ids=["invalid-utf8", "nul", "crlf", "missing-final-newline"],
    )
    def test_patch_and_plain_text_keep_byte_and_line_ending_validation(
        self, tmp_path: Path, suffix: str, data: bytes, message: str
    ) -> None:
        """Patch handling preserves the UTF-8, NUL, LF and final-newline contract."""
        root = tmp_path
        path = root / f"example{suffix}"
        path.write_bytes(data)
        with (
            mock.patch.object(source_check, "ROOT", root),
            pytest.raises(SystemExit, match=message),
        ):
            source_check.check_text([path])

# SPDX-License-Identifier: GPL-2.0-only
"""Host component tests for source text validation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check as source_check


class SourceTextTests(unittest.TestCase):
    """Validate real file bytes without changing unified-diff syntax."""

    def test_patch_accepts_an_empty_context_row_without_rewriting_it(self) -> None:
        """An empty context line retains its required single-space marker."""
        data = b"--- a/example.txt\n+++ b/example.txt\n@@ -1,2 +1,2 @@\n \n-old\n+new\n"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "example.patch"
            path.write_bytes(data)
            with mock.patch.object(source_check, "ROOT", root):
                source_check.check_text([path])
            self.assertEqual(path.read_bytes(), data)

    def test_trailing_whitespace_is_rejected_outside_an_empty_patch_context_row(self) -> None:
        """The diff marker exception must not hide whitespace in actual payloads."""
        cases = (
            ("example.txt", b" \n"),
            ("example.patch", b"  \n"),
            ("example.patch", b"+new \n"),
            ("example.patch", b"+new\t\n"),
            ("example.patch", b" context \n"),
            ("example.patch", b"-old\t\n"),
        )
        for name, data in cases:
            with self.subTest(name=name, data=data), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                path = root / name
                path.write_bytes(data)
                with (
                    mock.patch.object(source_check, "ROOT", root),
                    self.assertRaisesRegex(SystemExit, "trailing whitespace"),
                ):
                    source_check.check_text([path])

    def test_patch_and_plain_text_keep_byte_and_line_ending_validation(self) -> None:
        """Patch handling preserves the UTF-8, NUL, LF and final-newline contract."""
        cases = (
            (b"\xff\n", "source is not UTF-8"),
            (b"\0\n", "NUL byte in source file"),
            (b"text\r\n", "non-LF line ending"),
            (b"text", "missing final newline"),
        )
        for suffix in (".txt", ".patch"):
            for data, message in cases:
                with (
                    self.subTest(suffix=suffix, data=data),
                    tempfile.TemporaryDirectory() as temporary,
                ):
                    root = Path(temporary)
                    path = root / f"example{suffix}"
                    path.write_bytes(data)
                    with (
                        mock.patch.object(source_check, "ROOT", root),
                        self.assertRaisesRegex(SystemExit, message),
                    ):
                        source_check.check_text([path])


if __name__ == "__main__":
    unittest.main()

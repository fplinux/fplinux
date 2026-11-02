# SPDX-License-Identifier: GPL-2.0-only
"""File hashing and atomic publication on a temporary host filesystem."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.common import (
    canonical_json_bytes,
    read_json_object,
    replace_file_atomically,
    sha256_file,
)


class FileDigestTests:
    """Check SHA-256 against fixed vectors independent of the implementation."""

    @pytest.mark.parametrize(
        ("contents", "expected"),
        [
            (b"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
            (b"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
            (b"a" * 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
        ],
        ids=["empty", "short", "multibuffer"],
    )
    def test_empty_short_and_multibuffer_files_match_sha256_vectors(
        self, contents: bytes, expected: str
    ) -> None:
        """Whole-file hashing includes every byte, including across read buffers."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input"
            path.write_bytes(contents)
            assert (sha256_file(path)) == (expected)


class JsonReceiptTests:
    """Check the fixed bytes and cache-miss behavior of shared JSON receipts."""

    def test_canonical_json_uses_sorted_ascii_compact_utf8_with_a_trailing_newline(self) -> None:
        """One literal receipt documents the stable on-disk representation."""
        value = {"z": ["☃", "line\nbreak"], "a": 2}

        assert (canonical_json_bytes(value)) == (b'{"a":2,"z":["\\u2603","line\\nbreak"]}\n')

    def test_json_object_reader_returns_only_valid_objects(self) -> None:
        """A missing, malformed, or non-object receipt is a cache miss."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "receipt.json"

            assert (read_json_object(path)) is None
            path.write_bytes(b'{"answer":42}')
            assert (read_json_object(path)) == ({"answer": 42})
            path.write_bytes(b"[]")
            assert (read_json_object(path)) is None
            path.write_bytes(b"{")
            assert (read_json_object(path)) is None


class AtomicPublicationTests:
    """Preserve complete bytes and publication modes across replacement and failure."""

    @pytest.mark.parametrize("mode", [384, 420], ids=["0o600", "0o644"])
    def test_replacement_sets_requested_mode_and_leaves_no_temporary_file(self, mode: int) -> None:
        """Published files receive their caller's mode rather than the previous file's."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / "output"
            path.write_bytes(b"old")
            path.chmod(0o755)

            replace_file_atomically(path, b"replacement\x00bytes\n", mode)

            assert (path.read_bytes()) == (b"replacement\x00bytes\n")
            assert (path.stat().st_mode & 0o777) == (mode)
            assert (list(directory.iterdir())) == ([path])

    def test_default_sync_barrier_precedes_publication(self) -> None:
        """The old file remains visible until the complete replacement is synced."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "output"
            path.write_bytes(b"old")
            synced: list[bytes] = []
            real_fsync = os.fsync

            def observe_sync(descriptor: int) -> None:
                assert (path.read_bytes()) == (b"old")
                synced.append(os.pread(descriptor, 32, 0))
                real_fsync(descriptor)

            with mock.patch("fplinux_cli.common.os.fsync", side_effect=observe_sync):
                replace_file_atomically(path, b"complete replacement", 0o600)

            assert (synced) == ([b"complete replacement"])
            assert (path.read_bytes()) == (b"complete replacement")

    def test_sync_failure_preserves_previous_file_and_removes_partial_output(self) -> None:
        """A failed required sync cannot publish new contents or leave a staging file."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / "output"
            path.write_bytes(b"old")

            with (
                mock.patch("fplinux_cli.common.os.fsync", side_effect=OSError("sync failed")),
                pytest.raises(OSError, match="sync failed"),
            ):
                replace_file_atomically(path, b"new", 0o600)

            assert (path.read_bytes()) == (b"old")
            assert (list(directory.iterdir())) == ([path])

    def test_sync_can_be_skipped_for_replace_only_publication(self) -> None:
        """Callers without a durability barrier still publish complete bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / "output"
            path.write_bytes(b"old")

            with mock.patch("fplinux_cli.common.os.fsync", side_effect=OSError("sync failed")):
                replace_file_atomically(path, b"new", 0o600, sync=False)

            assert (path.read_bytes()) == (b"new")
            assert (list(directory.iterdir())) == ([path])

    def test_failed_replacement_keeps_destination_and_cleans_staging(self) -> None:
        """An OS rename rejection preserves existing directory contents."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / "output"
            path.mkdir()
            sentinel = path / "sentinel"
            sentinel.write_bytes(b"keep")

            # OS rejection details vary by host; the contract is failure and preserved contents.
            with pytest.raises(OSError):  # noqa: PT011
                replace_file_atomically(path, b"new", 0o600)

            assert (sentinel.read_bytes()) == (b"keep")
            assert (list(directory.iterdir())) == ([path])

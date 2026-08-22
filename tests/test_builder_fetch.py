# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for atomic replacement of pinned download-cache entries."""

from __future__ import annotations

import hashlib
import io
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

from fplinux_cli import builder


class BuilderFetchTests(unittest.TestCase):
    """A failed refresh must not remove an older verified or inspectable cache file."""

    def setUp(self) -> None:
        """Create an existing cache entry whose contents must survive failures."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.cache = Path(self.temporary.name) / "downloads"
        self.destination = self.cache / "linux.tar.xz"
        self.cache.mkdir()
        self.destination.write_bytes(b"old bytes\n")

    def test_network_failure_preserves_the_previous_destination(self) -> None:
        """Network errors leave the previous cache entry untouched."""
        expected = hashlib.sha256(b"new bytes\n").hexdigest()
        with (
            mock.patch(
                "fplinux_cli.builder.urllib.request.urlopen",
                side_effect=urllib.error.URLError("offline"),
            ),
            self.assertRaises(urllib.error.URLError),
        ):
            builder.fetch(
                "https://example.invalid/linux.tar.xz", expected, self.cache, "linux.tar.xz"
            )

        self.assertEqual(self.destination.read_bytes(), b"old bytes\n")

    def test_digest_failure_preserves_the_previous_destination(self) -> None:
        """Digest mismatches leave the previous cache entry untouched."""
        expected = hashlib.sha256(b"new bytes\n").hexdigest()
        response = io.BytesIO(b"wrong bytes\n")
        with (
            mock.patch("fplinux_cli.builder.urllib.request.urlopen", return_value=response),
            self.assertRaises(SystemExit),
        ):
            builder.fetch(
                "https://example.invalid/linux.tar.xz", expected, self.cache, "linux.tar.xz"
            )

        self.assertEqual(self.destination.read_bytes(), b"old bytes\n")

    def test_verified_download_replaces_destination_after_digest_match(self) -> None:
        """Only a verified temporary download replaces the cache entry."""
        expected_bytes = b"new bytes\n"
        expected = hashlib.sha256(expected_bytes).hexdigest()
        response = io.BytesIO(expected_bytes)
        with mock.patch("fplinux_cli.builder.urllib.request.urlopen", return_value=response):
            result = builder.fetch(
                "https://example.invalid/linux.tar.xz", expected, self.cache, "linux.tar.xz"
            )

        self.assertEqual(result, self.destination)
        self.assertEqual(self.destination.read_bytes(), expected_bytes)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for atomic replacement of pinned download-cache entries."""

from __future__ import annotations

import hashlib
import io
import tempfile
import urllib.error
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.alpine import packages as alpine_builder
from fplinux_cli.build import sources as sources_build

if TYPE_CHECKING:
    from collections.abc import Iterator


class BuilderFetchTests:
    """A failed refresh must not remove an older verified or inspectable cache file."""

    @pytest.fixture(autouse=True)
    def cache_entry(self) -> Iterator[None]:
        """Create an existing cache entry whose contents must survive failures."""
        self.temporary = tempfile.TemporaryDirectory()
        with self.temporary:
            self.cache = Path(self.temporary.name) / "downloads"
            self.destination = self.cache / "linux.tar.xz"
            self.cache.mkdir()
            self.destination.write_bytes(b"old bytes\n")
            yield

    def test_network_failure_preserves_the_previous_destination(self) -> None:
        """Network errors leave the previous cache entry untouched."""
        expected = hashlib.sha256(b"new bytes\n").hexdigest()
        with (
            mock.patch(
                "fplinux_cli.build.sources.urllib.request.urlopen",
                side_effect=urllib.error.URLError("offline"),
            ),
            pytest.raises(urllib.error.URLError),
        ):
            sources_build.fetch(
                "https://example.invalid/linux.tar.xz", expected, self.cache, "linux.tar.xz"
            )

        assert (self.destination.read_bytes()) == (b"old bytes\n")

    def test_digest_failure_preserves_the_previous_destination(self) -> None:
        """Digest mismatches leave the previous cache entry untouched."""
        expected = hashlib.sha256(b"new bytes\n").hexdigest()
        response = io.BytesIO(b"wrong bytes\n")
        with (
            mock.patch("fplinux_cli.build.sources.urllib.request.urlopen", return_value=response),
            pytest.raises(SystemExit),
        ):
            sources_build.fetch(
                "https://example.invalid/linux.tar.xz", expected, self.cache, "linux.tar.xz"
            )

        assert (self.destination.read_bytes()) == (b"old bytes\n")

    def test_verified_download_replaces_destination_after_digest_match(self) -> None:
        """Only a verified temporary download replaces the cache entry."""
        expected_bytes = b"new bytes\n"
        expected = hashlib.sha256(expected_bytes).hexdigest()
        response = io.BytesIO(expected_bytes)
        with mock.patch("fplinux_cli.build.sources.urllib.request.urlopen", return_value=response):
            result = sources_build.fetch(
                "https://example.invalid/linux.tar.xz", expected, self.cache, "linux.tar.xz"
            )

        assert (result) == (self.destination)
        assert (self.destination.read_bytes()) == (expected_bytes)


class AlpineArtifactFetchTests:
    """Locked Alpine consumers keep exact bytes, size checks and cache reuse."""

    def test_locked_package_download_is_reused_without_network(self) -> None:
        """The package consumer accepts exact bytes and reuses a verified cache entry."""
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            contents = b"locked package\n"
            lock = {"repositories": {"main": "https://example.invalid/main"}}
            records: dict[str, dict[str, object]] = {
                "example.apk": {
                    "repository": "main",
                    "sha256": hashlib.sha256(contents).hexdigest(),
                    "bytes": len(contents),
                }
            }
            with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(contents)):
                package = alpine_builder._locked_alpine_artifact(  # noqa: SLF001
                    lock, records, "example.apk", cache=cache
                )
            assert (package.read_bytes()) == (contents)
            with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
                reused = alpine_builder._locked_alpine_artifact(  # noqa: SLF001
                    lock, records, "example.apk", cache=cache
                )
            assert (reused) == (package)

            records["example.apk"]["bytes"] = 1
            with pytest.raises(SystemExit, match=r"Alpine package size mismatch for example.apk"):
                alpine_builder._locked_alpine_artifact(  # noqa: SLF001
                    lock, records, "example.apk", cache=cache
                )

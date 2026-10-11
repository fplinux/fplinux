# SPDX-License-Identifier: GPL-2.0-only
"""Locked Alpine package downloads and verified cache reuse."""

from __future__ import annotations

import hashlib
import io
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.alpine import packages as alpine_builder


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

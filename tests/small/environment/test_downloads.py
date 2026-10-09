# SPDX-License-Identifier: GPL-2.0-only
"""Download publication with controlled HTTPS responses and real temporary files."""

from __future__ import annotations

import io
import urllib.error
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.sources import fetch
from fplinux_cli.environment.downloads import download_locked_file

if TYPE_CHECKING:
    from pathlib import Path

_URL = "https://example.invalid/locked.bin"
_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
_ABC_SHA512 = (
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
)


def _download(consumer: str, destination: Path) -> Path:
    """Invoke either existing consumer without replacing its implementation."""
    if consumer == "source":
        return fetch(_URL, _ABC_SHA256, destination.parent, destination.name)
    return download_locked_file(_URL, _ABC_SHA256, destination)


@pytest.mark.parametrize("consumer", ["source", "locked"])
def test_verified_refresh_publishes_complete_private_bytes_and_reuses_them(
    tmp_path: Path, consumer: str
) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    destination.chmod(0o640)
    with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"abc")):
        assert _download(consumer, destination) == destination
    assert destination.read_bytes() == b"abc"
    assert destination.stat().st_mode & 0o777 == 0o600
    assert list(tmp_path.iterdir()) == [destination]
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        assert _download(consumer, destination) == destination


@pytest.mark.parametrize("consumer", ["source", "locked"])
def test_checksum_failure_preserves_old_bytes_and_removes_failed_temporary(
    tmp_path: Path, consumer: str
) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    destination.chmod(0o640)
    with (
        mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"unverified response")),
        pytest.raises(SystemExit, match="SHA256 mismatch"),
    ):
        _download(consumer, destination)
    assert destination.read_bytes() == b"previous declaration"
    assert destination.stat().st_mode & 0o777 == 0o640
    assert list(tmp_path.iterdir()) == [destination]


@pytest.mark.parametrize("consumer", ["source", "locked"])
def test_network_failure_preserves_old_bytes_and_the_consumer_error(
    tmp_path: Path, consumer: str
) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    error = urllib.error.URLError("controlled network failure")
    expected_error = urllib.error.URLError if consumer == "source" else SystemExit
    with (
        mock.patch("urllib.request.urlopen", side_effect=error),
        pytest.raises(expected_error) as failure,
    ):
        _download(consumer, destination)
    if consumer == "source":
        assert failure.value is error
    else:
        assert str(failure.value) == (
            "fplinux: locked download failed: https://example.invalid/locked.bin: "
            "<urlopen error controlled network failure>"
        )
    assert destination.read_bytes() == b"previous declaration"
    assert list(tmp_path.iterdir()) == [destination]


def test_locked_size_failure_keeps_previous_bytes_after_checksum_matches(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    with (
        mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"abc")),
        pytest.raises(SystemExit) as failure,
    ):
        download_locked_file(_URL, _ABC_SHA256, destination, size=2)
    assert str(failure.value) == (
        "fplinux: locked download size mismatch: https://example.invalid/locked.bin: expected 2"
    )
    assert destination.read_bytes() == b"previous declaration"
    assert list(tmp_path.iterdir()) == [destination]


def test_locked_checksum_error_precedes_size_error(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    with (
        mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"")),
        pytest.raises(SystemExit, match="locked download SHA256 mismatch"),
    ):
        download_locked_file(_URL, _ABC_SHA256, destination, size=2)
    assert list(tmp_path.iterdir()) == []


def test_offline_size_mismatch_requires_exact_saved_bytes_without_network(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"abc")
    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")),
        pytest.raises(SystemExit, match="offline locked input is missing or mismatched"),
    ):
        download_locked_file(_URL, _ABC_SHA256, destination, offline=True, size=2)
    assert destination.read_bytes() == b"abc"
    assert list(tmp_path.iterdir()) == [destination]


def test_native_sha512_is_verified_and_reused_offline(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"abc")):
        download_locked_file(_URL, _ABC_SHA512, destination, algorithm="sha512", size=3)
    assert destination.read_bytes() == b"abc"
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        assert (
            download_locked_file(
                _URL, _ABC_SHA512, destination, algorithm="sha512", offline=True, size=3
            )
            == destination
        )


def test_native_sha512_mismatch_does_not_replace_saved_bytes(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    with (
        mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"wrong bytes")),
        pytest.raises(SystemExit, match="locked download SHA512 mismatch"),
    ):
        download_locked_file(_URL, _ABC_SHA512, destination, algorithm="sha512")
    assert destination.read_bytes() == b"previous declaration"
    assert list(tmp_path.iterdir()) == [destination]

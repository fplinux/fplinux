# SPDX-License-Identifier: GPL-2.0-only
"""Download publication with controlled HTTPS responses and real temporary files."""

from __future__ import annotations

import io
import urllib.error
from email.message import Message
from http.client import IncompleteRead
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.sources import fetch
from fplinux_cli.environment.downloads import download_locked_file, transfer_verified_file

if TYPE_CHECKING:
    from pathlib import Path

_URL = "https://example.invalid/locked.bin"
_ABC_SHA256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
_ABC_SHA512 = (
    "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"
)


class _InterruptedResponse(io.BytesIO):
    """Deliver a prefix before the controlled HTTPS response breaks."""

    def __init__(self, value: bytes, *, failure: Exception) -> None:
        super().__init__(value)
        self.failure = failure

    def read(self, size: int | None = -1) -> bytes:
        if self.tell():
            raise self.failure
        return super().read(size)


@pytest.mark.parametrize(
    "failure_type", [IncompleteRead, TimeoutError], ids=["truncated", "timeout"]
)
@pytest.mark.parametrize("interruptions", [1, 2])
def test_interrupted_download_restarts_before_verifying_and_publishing(
    tmp_path: Path, interruptions: int, failure_type: type[Exception]
) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    destination.chmod(0o640)
    responses = [
        *(
            _InterruptedResponse(b"longer unverified prefix", failure=failure_type(b"interrupted"))
            for _ in range(interruptions)
        ),
        io.BytesIO(b"abc"),
    ]
    with mock.patch("urllib.request.urlopen", side_effect=responses) as request:
        assert download_locked_file(_URL, _ABC_SHA256, destination) == destination

    assert request.call_count == interruptions + 1
    assert all(response.closed for response in responses)
    assert destination.read_bytes() == b"abc"
    assert destination.stat().st_mode & 0o777 == 0o600
    assert list(tmp_path.iterdir()) == [destination]


@pytest.mark.parametrize(
    "failure_type", [IncompleteRead, TimeoutError], ids=["truncated", "timeout"]
)
def test_repeatedly_interrupted_download_stops_and_preserves_previous_bytes(
    tmp_path: Path, failure_type: type[Exception]
) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    destination.chmod(0o640)
    responses = [
        _InterruptedResponse(b"unverified prefix", failure=failure_type(b"interrupted"))
        for _ in range(3)
    ]
    with (
        mock.patch("urllib.request.urlopen", side_effect=responses) as request,
        pytest.raises(SystemExit, match="HTTP response interrupted after 3 attempts"),
    ):
        download_locked_file(_URL, _ABC_SHA256, destination)

    assert request.call_count == 3
    assert all(response.closed for response in responses)
    assert destination.read_bytes() == b"previous declaration"
    assert destination.stat().st_mode & 0o777 == 0o640
    assert list(tmp_path.iterdir()) == [destination]


@pytest.mark.parametrize(
    "failure_type", [IncompleteRead, TimeoutError], ids=["truncated", "timeout"]
)
def test_interrupted_metadata_transfer_reports_an_os_error_without_publishing(
    tmp_path: Path, failure_type: type[Exception]
) -> None:
    destination = tmp_path / "metadata.json"
    destination.write_bytes(b"previous metadata")
    responses = [
        _InterruptedResponse(b"unverified metadata", failure=failure_type(b"interrupted"))
        for _ in range(3)
    ]
    with (
        mock.patch("urllib.request.urlopen", side_effect=responses) as request,
        pytest.raises(OSError, match="HTTP response interrupted after 3 attempts"),
    ):
        transfer_verified_file(
            _URL, destination, verify=lambda _path: pytest.fail("incomplete bytes were verified")
        )

    assert request.call_count == 3
    assert all(response.closed for response in responses)
    assert destination.read_bytes() == b"previous metadata"
    assert list(tmp_path.iterdir()) == [destination]


def test_missing_http_input_is_not_retried(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    error = urllib.error.HTTPError(_URL, 404, "Not Found", Message(), None)
    with (
        mock.patch("urllib.request.urlopen", side_effect=error) as request,
        pytest.raises(SystemExit, match="HTTP Error 404: Not Found"),
    ):
        download_locked_file(_URL, _ABC_SHA256, destination)

    request.assert_called_once()
    assert list(tmp_path.iterdir()) == []


def test_verified_refresh_publishes_complete_private_bytes_and_reuses_them(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    destination.chmod(0o640)
    with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(b"abc")):
        assert download_locked_file(_URL, _ABC_SHA256, destination) == destination
    assert destination.read_bytes() == b"abc"
    assert destination.stat().st_mode & 0o777 == 0o600
    assert list(tmp_path.iterdir()) == [destination]
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        assert download_locked_file(_URL, _ABC_SHA256, destination) == destination


def test_checksum_failure_preserves_old_bytes_and_removes_failed_temporary(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    destination.chmod(0o640)
    with (
        mock.patch(
            "urllib.request.urlopen", return_value=io.BytesIO(b"unverified response")
        ) as request,
        pytest.raises(SystemExit, match="SHA256 mismatch"),
    ):
        download_locked_file(_URL, _ABC_SHA256, destination)
    request.assert_called_once()
    assert destination.read_bytes() == b"previous declaration"
    assert destination.stat().st_mode & 0o777 == 0o640
    assert list(tmp_path.iterdir()) == [destination]


def test_network_failure_preserves_old_bytes_and_the_consumer_error(tmp_path: Path) -> None:
    destination = tmp_path / "locked.bin"
    destination.write_bytes(b"previous declaration")
    error = urllib.error.URLError("controlled network failure")
    with (
        mock.patch("urllib.request.urlopen", side_effect=error) as request,
        pytest.raises(SystemExit) as failure,
    ):
        download_locked_file(_URL, _ABC_SHA256, destination)
    request.assert_called_once()
    assert str(failure.value) == (
        "fplinux: locked download failed: https://example.invalid/locked.bin: "
        "<urlopen error controlled network failure>"
    )
    assert destination.read_bytes() == b"previous declaration"
    assert list(tmp_path.iterdir()) == [destination]


@pytest.mark.parametrize("contents", [b"abc", None, b"wrong"], ids=["hit", "missing", "mismatch"])
def test_native_artifact_consumer_requires_exact_saved_bytes_without_network(
    tmp_path: Path,
    contents: bytes | None,
) -> None:
    destination = tmp_path / "locked.bin"
    if contents is not None:
        destination.write_bytes(contents)
    with mock.patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
        if contents == b"abc":
            assert fetch(_URL, _ABC_SHA256, tmp_path, "locked.bin") == destination
        else:
            with pytest.raises(SystemExit, match="offline locked input is missing or mismatched"):
                fetch(_URL, _ABC_SHA256, tmp_path, "locked.bin")
    if contents is not None:
        assert destination.read_bytes() == contents
    assert list(tmp_path.iterdir()) == ([destination] if contents is not None else [])


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

# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks of byte progress during direct SSH file streaming."""

from __future__ import annotations

import os
import signal
import sys
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime import ssh_transport

from tests import ROOT
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path
    from typing import Any, BinaryIO


@pytest.fixture
def local_stream(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> Callable[[str, BinaryIO, float, Callable[[int], None]], None]:
    """Replace only the SSH executable; keep real session, file and child ownership."""
    runtime = tmp_path / "runtime"
    runtime.mkdir(mode=0o700)
    session = create_ready_session(runtime)

    def stream(
        mode: str, destination: BinaryIO, timeout: float, progress: Callable[[int], None]
    ) -> None:
        def local_ssh(_session: dict[str, Any], _command: str) -> list[str]:
            return [
                sys.executable,
                str(ROOT / "tests" / "fixtures" / "ssh_transport" / "progress_writer.py"),
                mode,
                str(tmp_path / "release"),
                str(tmp_path / "pid"),
                str(tmp_path / "complete"),
            ]

        monkeypatch.setattr(ssh_transport, "_ssh_argv", local_ssh)
        ssh_transport.stream_remote(
            session, "binary read", destination, timeout=timeout, progress=progress
        )

    return stream


@pytest.mark.parametrize(
    ("initial", "position", "expected"),
    [
        (b"", 0, b"\x00\xffpart\x80end\n"),
        (b"prefix:", 7, b"prefix:\x00\xffpart\x80end\n"),
        (b"prefix:" + b"Z" * 32, 7, b"prefix:\x00\xffpart\x80end\n" + b"Z" * 21),
    ],
    ids=["empty", "append", "overwrite-without-size-growth"],
)
def test_stream_reports_written_bytes_before_completion_and_excludes_existing_content(
    tmp_path: Path,
    local_stream: Callable[[str, BinaryIO, float, Callable[[int], None]], None],
    initial: bytes,
    position: int,
    expected: bytes,
) -> None:
    """The first progress event releases the writer; final progress counts both chunks."""
    destination_path = tmp_path / "download"
    counts: list[int] = []

    def progress(count: int) -> None:
        counts.append(count)
        if count == 6:
            assert not (tmp_path / "complete").exists()
            (tmp_path / "release").touch()

    with destination_path.open("w+b") as destination:
        destination.write(initial)
        destination.seek(position)
        local_stream("success", destination, 5, progress)
        assert not destination.closed

    assert 6 in counts
    assert counts[-1] == 11
    assert counts == sorted(counts)
    assert destination_path.read_bytes() == expected


@pytest.mark.parametrize(
    ("mode", "timeout", "message", "expected", "final_count"),
    [
        ("failure", 5, "(?s)exit status 8:.*stream diagnostic", b"\x00\xffpart\x80end\n", 11),
        ("stall", 0.75, "timed out after 0.75s", b"\x00\xffpart", 6),
    ],
)
# Separate case inputs keep the expected bytes and diagnostic independent.
def test_stream_progress_preserves_partial_bytes_and_reaps_failed_or_timed_out_child(  # noqa: PLR0913
    *,
    tmp_path: Path,
    local_stream: Callable[[str, BinaryIO, float, Callable[[int], None]], None],
    mode: str,
    timeout: float,
    message: str,
    expected: bytes,
    final_count: int,
) -> None:
    """Diagnostics stay separate and a failed stream leaves no owned process running."""
    counts: list[int] = []

    def progress(count: int) -> None:
        counts.append(count)
        if count == 6:
            (tmp_path / "release").touch()

    destination_path = tmp_path / "download"
    previous_handlers = {
        signum: signal.getsignal(signum) for signum in (signal.SIGINT, signal.SIGTERM)
    }
    with (
        destination_path.open("w+b") as destination,
        pytest.raises(SystemExit, match=message),
    ):
        local_stream(mode, destination, timeout, progress)

    assert counts[-1] == final_count
    assert destination_path.read_bytes() == expected
    for signum, handler in previous_handlers.items():
        assert signal.getsignal(signum) == handler
    process_id = int((tmp_path / "pid").read_text(encoding="ascii"))
    with pytest.raises(ProcessLookupError):
        os.kill(process_id, 0)


def test_stream_callback_exception_terminates_child_and_keeps_destination_open(
    tmp_path: Path,
    local_stream: Callable[[str, BinaryIO, float, Callable[[int], None]], None],
) -> None:
    """An interrupted progress consumer cannot leave its binary writer alive."""

    def progress(count: int) -> None:
        if count == 6:
            message = "progress consumer stopped"
            raise RuntimeError(message)

    destination_path = tmp_path / "download"
    with destination_path.open("w+b") as destination:
        with pytest.raises(RuntimeError, match="progress consumer stopped"):
            local_stream("stall", destination, 5, progress)
        assert not destination.closed

    assert destination_path.read_bytes() == b"\x00\xffpart"
    process_id = int((tmp_path / "pid").read_text(encoding="ascii"))
    with pytest.raises(ProcessLookupError):
        os.kill(process_id, 0)

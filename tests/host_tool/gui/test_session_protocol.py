# SPDX-License-Identifier: GPL-2.0-only
"""Host session packets and receive errors over real seqpacket sockets."""

from __future__ import annotations

import shlex
from pathlib import Path

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process


def run_harness(tmp_path: Path, source: str, *, wraps: tuple[str, ...] = ()) -> None:
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve session JSON dependency",
        timeout=10,
        check=True,
    )
    executable = tmp_path / "session-protocol"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(Path(__file__).with_name(source)),
            str(ROOT / "lib/fplinux/fplinux-ui-session.c"),
            *(f"-Wl,--wrap={name}" for name in wraps),
            *shlex.split(flags.stdout),
        ],
        name=f"compile {source}",
    )
    run_process(
        [str(executable)],
        name=f"run {source}",
        timeout=20,
        check=True,
    )


def test_session_packets_and_descriptor_ownership(tmp_path: Path) -> None:
    """Literal packets, distinct languages and descriptor cleanup preserve the wire contract."""
    run_harness(tmp_path, "fplinux-session-protocol-test.c")


def test_session_receive_retries_eintr_and_reports_nonblocking_empty(tmp_path: Path) -> None:
    """One interrupted receive delivers the queued frame; an empty socket reports no packet."""
    run_harness(tmp_path, "fplinux-session-protocol-transport-test.c", wraps=("recvmsg",))

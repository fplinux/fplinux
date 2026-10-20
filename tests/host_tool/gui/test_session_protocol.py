# SPDX-License-Identifier: GPL-2.0-only
"""Host session packets over real seqpacket and a declared ancillary fault double."""

from __future__ import annotations

import shlex
from pathlib import Path

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process


@pytest.mark.parametrize("transport", ["seqpacket", "ancillary-fault-double"])
def test_session_packets_and_descriptor_ownership(tmp_path: Path, transport: str) -> None:
    """Literal packets, distinct languages and descriptor cleanup preserve the wire contract."""
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve session JSON dependency",
        timeout=10,
        check=True,
    )
    executable = tmp_path / "session-protocol"
    sources = [
        str(Path(__file__).with_name("fplinux-session-protocol-test.c")),
        str(ROOT / "lib/fplinux/fplinux-ui-session.c"),
    ]
    wraps: list[str] = []
    if transport == "ancillary-fault-double":
        sources.append(str(Path(__file__).with_name("fplinux-session-protocol-transport-test.c")))
        wraps = [
            f"-Wl,--wrap={name}" for name in ("socketpair", "send", "sendmsg", "recvmsg", "close")
        ]
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            *sources,
            *wraps,
            *shlex.split(flags.stdout),
        ],
        name=f"compile session {transport} harness",
    )
    run_process(
        [str(executable)],
        name=f"run session {transport} harness",
        timeout=20,
        check=True,
    )

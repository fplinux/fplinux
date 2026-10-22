# SPDX-License-Identifier: GPL-2.0-only
"""Literal Clock messages linked to the production codec and session owner."""

from __future__ import annotations

import shlex
from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_clock_messages_and_retained_owner_state(tmp_path: Path) -> None:
    """Strict JSON, exact event IDs and user values cross the real wire codec."""
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve Clock JSON dependency",
        timeout=10,
        check=True,
    )
    aport = ROOT / "alpine/aports/fplinux-clocks"
    session = ROOT / "alpine/aports/fplinux-session"
    executable = tmp_path / "clock-wire"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{aport}",
            f"-I{session}",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/clock-wire-test.c"),
            str(aport / "clock-model.c"),
            str(aport / "clock-wire.c"),
            str(session / "session-clock.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            *shlex.split(flags.stdout),
        ],
        name="compile Clock wire and owner behavior harness",
    )
    result = run_process(
        [
            str(executable),
            str(tmp_path / "requests"),
            str(tmp_path / "snapshot"),
            str(ROOT / "tests/host_tool/gui/fixtures/clock-world"),
        ],
        name="run Clock wire and owner behavior",
        timeout=10,
        check=True,
    )
    assert result.stdout == "clock-wire: strict messages and owner state passed\n"
    assert result.stderr == ""

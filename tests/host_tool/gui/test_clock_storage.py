# SPDX-License-Identifier: GPL-2.0-only
"""Real owner preferences retain configuration while failed writes stay unacknowledged."""

from __future__ import annotations

import errno
import os
import shlex
from typing import TYPE_CHECKING

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


def test_clock_owner_durable_configuration_and_failed_write_retry(tmp_path: Path) -> None:
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve Clock storage JSON dependency",
        timeout=10,
        check=True,
    )
    clocks = ROOT / "alpine/aports/fplinux-clocks"
    session = ROOT / "alpine/aports/fplinux-session"
    executable = tmp_path / "clock-storage"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{clocks}",
            f"-I{session}",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/clock-storage-test.c"),
            str(clocks / "clock-model.c"),
            str(clocks / "clock-wire.c"),
            str(session / "session-clock.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            "-Wl,--wrap=fsync",
            *shlex.split(flags.stdout),
        ],
        name="compile Clock durable owner behavior harness",
    )
    result = run_process(
        [
            str(executable),
            str(tmp_path / "clocks"),
            str(ROOT / "tests/host_tool/gui/fixtures/clock-world"),
        ],
        name="run Clock durable owner behavior",
        timeout=10,
        check=True,
    )
    assert (
        result.stdout == "clock-storage: durable configuration and transactional failures passed\n"
    )
    cause = os.strerror(errno.EIO)
    assert result.stderr == (
        f"fplinux-session: clock checkpoint not saved: {cause}\n"
        "fplinux-session: clock checkpoint saved\n"
        f"fplinux-session: clock checkpoint not saved: {cause}\n"
        "fplinux-session: clock checkpoint saved\n"
        f"fplinux-session: clock checkpoint saving not confirmed: {cause}\n"
        "fplinux-session: clock checkpoint saved\n"
        f"fplinux-session: clock checkpoint not saved: {cause}\n"
        "fplinux-session: clock checkpoint saved\n"
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Host media control packets over a real Unix seqpacket connection."""

from __future__ import annotations

import shlex
from pathlib import Path

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process


def test_ui_media_capabilities_and_vibration_packets(tmp_path: Path) -> None:
    """Capabilities, duration bounds and malformed packets preserve the media contract."""
    flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve media protocol JSON dependency",
        timeout=10,
        check=True,
    )
    executable = tmp_path / "ui-media-protocol"
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            str(Path(__file__).with_name("ui-media-protocol-test.c")),
            str(ROOT / "lib/fplinux/fplinux-ui-session.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            *shlex.split(flags.stdout),
        ],
        name="compile UI media protocol harness",
    )
    run_process(
        [str(executable)],
        name="run UI media protocol harness",
        timeout=20,
        check=True,
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Host launch-card ownership and bounded spinner rendering."""

from pathlib import Path

from tests import ROOT
from tests.c_harness import compile_c
from tests.process import run_process


def test_launch_card_copy_and_spinner_pixels(tmp_path: Path) -> None:
    """Borrowed descriptors retain owned pixels; invalid images clear old storage."""
    aport = ROOT / "alpine/aports/fplinux-session"
    executable = tmp_path / "session-launch-surface"
    mark = tmp_path / "mark.rgb565"
    mark.write_bytes(bytes.fromhex("00 f8 e0 07 1f 00 ff ff"))
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(aport),
            "-I",
            str(ROOT / "include/fplinux"),
            str(Path(__file__).with_name("fplinux-session-launch-surface-test.c")),
            str(aport / "session-launch-surface.c"),
        ],
        name="compile launch-card host component",
    )
    run_process(
        [str(executable), str(mark)],
        name="run launch-card host component",
        timeout=10,
        check=True,
    )

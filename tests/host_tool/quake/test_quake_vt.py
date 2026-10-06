# SPDX-License-Identifier: GPL-2.0-only
"""Quake event pump with real signalfd dispatch and fake engine/device boundaries."""

from __future__ import annotations

import shlex
import tempfile
import unittest
from pathlib import Path

from tests import ROOT
from tests.process import run_process


class QuakeVtTests(unittest.TestCase):
    """Observe ownership changes without drawing, running the game or opening a VT."""

    def test_event_pump_releases_and_reacquires_input_without_a_frame(self) -> None:
        """A modal event loop must service display handoff before processing input."""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "quake-vt"
            flags = shlex.split(
                run_process(
                    ["pkg-config", "--cflags", "--libs", "libdrm"],
                    name="read DRM compiler and linker flags",
                    timeout=10,
                    check=True,
                ).stdout
            )
            run_process(
                [
                    "cc",
                    "-std=gnu11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-DNQ_HACK",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'tests/host_tool/quake/quake-video-compat'}",
                    f"-I{ROOT / 'alpine/aports/fplinux-tyrquake'}",
                    str(ROOT / "tests/host_tool/quake/fplinux-quake-vt.c"),
                    str(ROOT / "alpine/aports/fplinux-tyrquake/vid_fplinux.c"),
                    str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                    (
                        "-Wl,--wrap=drmDropMaster,--wrap=drmSetMaster,--wrap=ioctl,"
                        "--wrap=fplinux_drm_session_open,--wrap=fplinux_drm_session_close"
                    ),
                    *flags,
                    "-o",
                    str(executable),
                ],
                name="compile Quake event pump with fake engine and device boundaries",
                timeout=30,
                check=True,
            )
            run_process(
                [str(executable)],
                name="observe Quake input ownership without rendering",
                timeout=5,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()

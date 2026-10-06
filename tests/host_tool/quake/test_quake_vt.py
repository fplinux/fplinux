# SPDX-License-Identifier: GPL-2.0-only
"""Quake event pump with real signalfd dispatch and fake engine/device boundaries."""

from __future__ import annotations

import shlex
import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class QuakeVtTests:
    """Observe ownership changes without drawing, running the game or opening a VT."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            directory = build_directory.name
            cls.executable = Path(directory) / "quake-vt"
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
                    str(cls.executable),
                ],
                name="compile Quake event pump with fake engine and device boundaries",
                timeout=30,
                check=True,
            )
            yield

    def test_event_pump_releases_and_reacquires_input_without_a_frame(self) -> None:
        """A modal event loop must service display handoff before processing input."""
        run_process(
            [str(self.executable)],
            name="observe Quake input ownership without rendering",
            timeout=5,
            check=True,
        )

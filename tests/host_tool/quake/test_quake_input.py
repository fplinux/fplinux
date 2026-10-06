# SPDX-License-Identifier: GPL-2.0-only
"""Native Quake input mapping with fake engine, display and input-session boundaries."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


class QuakeInputTests:
    """Observe engine key states without running the game, DRM or evdev devices."""

    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            temporary = build_directory.name
            cls.executable = Path(temporary) / "quake-input"
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'tests/host_tool/quake/quake-input-compat'}",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'alpine/aports/fplinux-tyrquake'}",
                    str(ROOT / "alpine/aports/fplinux-tyrquake/in_fplinux.c"),
                    str(ROOT / "tests/host_tool/quake/fplinux-quake-input.c"),
                    "-o",
                    str(cls.executable),
                ],
                name="compile Quake input with fake runtime boundaries",
                timeout=30,
                check=True,
            )
            yield

    def test_sources_and_shared_actions_survive_releases_and_focus_reset(self) -> None:
        """Releasing one key cannot stop an action still held by another key."""
        run_process(
            [str(self.executable)],
            name="observe Quake key mapping and held actions",
            timeout=5,
            check=True,
        )

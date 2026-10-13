# SPDX-License-Identifier: GPL-2.0-only
"""Native Quake input mapping with fake engine, display and input-session boundaries."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


class QuakeInputTests:
    """Observe engine key states without running the game, DRM or evdev devices."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "quake-input") as directory:
            executable = directory / "quake-input"
            compile_c(
                executable,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'tests/host_tool/quake/quake-input-compat'}",
                    f"-I{ROOT / 'include/fplinux'}",
                    f"-I{ROOT / 'alpine/aports/fplinux-tyrquake'}",
                    str(ROOT / "alpine/aports/fplinux-tyrquake/in_fplinux.c"),
                    str(ROOT / "tests/host_tool/quake/fplinux-quake-input.c"),
                ],
                name="compile Quake input with fake runtime boundaries",
                check=True,
            )
            yield executable

    @pytest.mark.parametrize("orientation", ["portrait", "landscape"], ids=["240x320", "320x240"])
    def test_arrows_follow_display_and_shared_actions_survive_releases_and_focus_reset(
        self, executable: Path, orientation: str
    ) -> None:
        """D-pad arrows follow the display; a held action survives another key's release."""
        run_process(
            [str(executable), orientation],
            name="observe Quake key mapping and held actions",
            timeout=5,
            check=True,
        )

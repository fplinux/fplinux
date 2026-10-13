# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool checks for the production ARMADA scene and timeline."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-showcase"
HARNESS = ROOT / "tests/host_tool/display/fplinux-showcase.c"
SCENE = APORT / "armada-scene.c"


@dataclass(frozen=True)
class ShowcaseHarness:
    """Linked scene renderer and its two immutable fixture fonts."""

    executable: Path
    small_font: Path
    large_font: Path


class FplinuxShowcaseHostToolTests:
    """Render real frames without claiming framebuffer or phone coverage."""

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[ShowcaseHarness]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "fplinux-showcase") as temporary:
            executable = temporary / "fplinux-showcase-test"
            small_font = temporary / "small.psf"
            large_font = temporary / "large.psf"
            write_solid_ascii_font(small_font, width=6, height=12)
            write_solid_ascii_font(large_font, width=8, height=16)
            compile_c(
                executable,
                [
                    "-O2",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{APORT}",
                    f"-I{ROOT / 'include/fplinux'}",
                    str(HARNESS),
                    str(SCENE),
                    str(APORT / "armada-storyboard.c"),
                    str(APORT / "armada-renderer.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                ],
                name="compile FPLinux showcase host harness",
                check=True,
            )
            yield ShowcaseHarness(executable, small_font, large_font)

    def test_renderer_and_timeline_at_both_display_sizes(self, harness: ShowcaseHarness) -> None:
        """Protect bounds, frame-history independence, cues, and loop wrapping."""
        run_process(
            [str(harness.executable), str(harness.small_font), str(harness.large_font)],
            name="run FPLinux showcase host harness",
            timeout=30,
            check=True,
        )

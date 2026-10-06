# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool checks for the production ARMADA scene and timeline."""

from __future__ import annotations

import tempfile
from contextlib import ExitStack
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.psf_font import write_solid_ascii_font
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

APORT = ROOT / "alpine/aports/fplinux-showcase"
HARNESS = ROOT / "tests/host_tool/display/fplinux-showcase.c"
SCENE = APORT / "armada-scene.c"


class FplinuxShowcaseHostToolTests:
    """Render real frames without claiming framebuffer or phone coverage."""

    executable: Path
    large_font: Path
    small_font: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Link the host harness once for this test group."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            temporary = build_directory.name
            cls.executable = Path(temporary) / "fplinux-showcase-test"
            cls.small_font = Path(temporary) / "small.psf"
            cls.large_font = Path(temporary) / "large.psf"
            write_solid_ascii_font(cls.small_font, width=6, height=12)
            write_solid_ascii_font(cls.large_font, width=8, height=16)
            run_process(
                [
                    "cc",
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
                    "-o",
                    str(cls.executable),
                ],
                name="compile FPLinux showcase host harness",
                timeout=30,
                check=True,
            )
            yield

    def test_renderer_and_timeline_at_both_display_sizes(self) -> None:
        """Protect bounds, frame-history independence, cues, and loop wrapping."""
        run_process(
            [str(self.executable), str(self.small_font), str(self.large_font)],
            name="run FPLinux showcase host harness",
            timeout=30,
            check=True,
        )

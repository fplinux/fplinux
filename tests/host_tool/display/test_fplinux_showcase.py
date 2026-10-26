# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool checks for the production ARMADA scene and timeline."""

from __future__ import annotations

import shlex
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
    output_executable: Path
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
            output_executable = temporary / "armada-output-test"
            small_font = temporary / "small.psf"
            large_font = temporary / "large.psf"
            write_solid_ascii_font(small_font, width=6, height=12)
            write_solid_ascii_font(large_font, width=8, height=16)
            drm_flags = shlex.split(
                run_process(
                    ["pkg-config", "--cflags", "--libs", "libdrm"],
                    name="read DRM flags for the Showcase GPU boundary",
                    timeout=10,
                    check=True,
                ).stdout
            )
            compile_c(
                executable,
                [
                    "-O2",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-isystem",
                    "/opt/quality/include",
                    f"-I{APORT}",
                    f"-I{ROOT / 'include/fplinux'}",
                    str(HARNESS),
                    str(SCENE),
                    str(APORT / "armada-storyboard.c"),
                    str(APORT / "armada-renderer.c"),
                    str(APORT / "armada-geometry.c"),
                    str(APORT / "armada-overlay.c"),
                    str(APORT / "armada-gpu-renderer.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                    str(ROOT / "lib/fplinux/fplinux-gpu.c"),
                    str(ROOT / "lib/fplinux/fplinux-gpu-output.c"),
                    str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                    "-ldl",
                    *drm_flags,
                ],
                name="compile FPLinux showcase host harness",
                check=True,
            )
            compile_c(
                output_executable,
                [
                    "-O2",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-isystem",
                    "/opt/quality/include",
                    f"-I{APORT}",
                    f"-I{ROOT / 'include/fplinux'}",
                    f'-DARMADA_SHADER_DIRECTORY="{APORT}"',
                    str(ROOT / "tests/host_tool/display/armada-output.c"),
                    str(SCENE),
                    str(APORT / "armada-storyboard.c"),
                    str(APORT / "armada-renderer.c"),
                    str(APORT / "armada-geometry.c"),
                    str(APORT / "armada-overlay.c"),
                    str(APORT / "armada-gpu-renderer.c"),
                    str(ROOT / "lib/fplinux/fplinux-font.c"),
                    str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                    "-Wl,--wrap=fopen",
                    *drm_flags,
                ],
                name="compile ARMADA native output host harness",
                check=True,
            )
            yield ShowcaseHarness(executable, output_executable, small_font, large_font)

    def test_software_renderer_and_timeline_at_supported_display_sizes(
        self, harness: ShowcaseHarness
    ) -> None:
        """Protect bounds, frame-history independence, cues, and loop wrapping."""
        run_process(
            [str(harness.executable), str(harness.small_font), str(harness.large_font)],
            name="run FPLinux showcase host harness",
            timeout=30,
            check=True,
        )

    @pytest.mark.parametrize("mode", ["auto", "software", "gpu"])
    @pytest.mark.parametrize("vertex_present", [False, True], ids=["no-shaders", "no-fragment"])
    def test_missing_optional_shaders_preserve_the_renderer_policy(
        self,
        harness: ShowcaseHarness,
        tmp_path: Path,
        mode: str,
        *,
        vertex_present: bool,
    ) -> None:
        """Real missing shader files allow CPU modes and fail a forced GPU scene."""
        if vertex_present:
            (tmp_path / "armada-scene.vert").write_bytes(
                (APORT / "armada-scene.vert").read_bytes()
            )

        run_process(
            [
                str(harness.output_executable),
                f"missing-shaders-{mode}",
                "240",
                "320",
                str(harness.large_font),
            ],
            name=f"render ARMADA in {mode} mode without optional shaders",
            timeout=10,
            env={"FPLINUX_ARMADA_TEST_SHADER_ROOT": str(tmp_path), "LC_ALL": "C"},
            check=True,
        )

    @pytest.mark.parametrize(("width", "height"), [(240, 320), (128, 160), (320, 240)])
    def test_gpu_scene_uses_the_padded_native_back_page(
        self, harness: ShowcaseHarness, width: int, height: int
    ) -> None:
        """Exercise actual scene objects with synthetic GPU and display output."""
        font = harness.small_font if width == 128 else harness.large_font
        run_process(
            [str(harness.output_executable), "render", str(width), str(height), str(font)],
            name=f"render ARMADA into a padded {width}x{height} native output",
            timeout=10,
            check=True,
        )

    @pytest.mark.parametrize(
        "scenario",
        [
            "output-failure",
            "forced-gpu-failure",
            "missing-procedure",
            "shader-failure",
            "link-failure",
            "target-failure",
            "cleanup-failure",
            "render-failure",
            "begin-failure",
            "end-failure",
        ],
    )
    def test_gpu_failure_cleanup_and_software_fallback(
        self, harness: ShowcaseHarness, scenario: str
    ) -> None:
        """Keep ownership ordered and stop failed frames before presentation."""
        run_process(
            [str(harness.output_executable), scenario, "240", "320", str(harness.large_font)],
            name=f"exercise ARMADA native output {scenario}",
            timeout=10,
            check=True,
        )

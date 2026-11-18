# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool checks for the production ARMADA scene and timeline."""

from __future__ import annotations

import shlex
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-showcase"
HARNESS = ROOT / "tests/host_tool/display/fplinux-showcase.c"
SCENE = APORT / "armada-scene.c"


@dataclass(frozen=True)
class ShowcaseHarness:
    """Linked scene renderer and controlled offscreen GPU boundary."""

    executable: Path
    output_executable: Path


class FplinuxShowcaseHostToolTests:
    """Render real frames without claiming framebuffer or phone coverage."""

    @pytest.fixture(scope="class")
    @classmethod
    def harness(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[ShowcaseHarness]:
        """Link the host harness once for this test group."""
        with c_build_directory(tmp_path_factory, "fplinux-showcase") as temporary:
            executable = temporary / "fplinux-showcase-test"
            output_executable = temporary / "armada-output-test"
            drm_flags = shlex.split(
                run_process(
                    ["pkg-config", "--cflags", "--libs", "libdrm"],
                    name="read render-node library flags for the Showcase GPU context",
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
                    str(APORT / "armada-gpu-renderer.c"),
                    str(ROOT / "lib/fplinux-shared/fplinux-gpu.c"),
                    str(ROOT / "lib/fplinux-shared/fplinux-gpu-frame.c"),
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
                    str(APORT / "armada-gpu-renderer.c"),
                    str(ROOT / "lib/fplinux-shared/fplinux-gpu-frame.c"),
                    "-Wl,--wrap=fopen",
                ],
                name="compile ARMADA offscreen GPU host harness",
                check=True,
            )
            yield ShowcaseHarness(executable, output_executable)

    @pytest.mark.parametrize(
        ("width", "height"),
        [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240), (237, 269), (127, 113)],
    )
    def test_software_renderer_and_timeline_at_supported_frame_sizes(
        self, harness: ShowcaseHarness, width: int, height: int
    ) -> None:
        """Protect bounds, frame-history independence, cues, and loop wrapping."""
        run_process(
            [str(harness.executable), str(width), str(height)],
            name=f"render ARMADA software frames at {width}x{height}",
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
            ],
            name=f"render ARMADA in {mode} mode without optional shaders",
            timeout=10,
            env={"FPLINUX_ARMADA_TEST_SHADER_ROOT": str(tmp_path), "LC_ALL": "C"},
            check=True,
        )

    @pytest.mark.parametrize(
        ("width", "height"),
        [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240), (237, 269), (127, 113)],
    )
    def test_gpu_readback_is_top_down_tight_rgb565(
        self, harness: ShowcaseHarness, width: int, height: int
    ) -> None:
        """Check literal asymmetric colors, row orientation, guards and buffer reuse."""
        run_process(
            [str(harness.output_executable), "render", str(width), str(height)],
            name=f"read ARMADA offscreen frame into tight {width}x{height} RGB565",
            timeout=10,
            check=True,
        )

    @pytest.mark.parametrize(
        "scenario",
        [
            "context-failure",
            "forced-gpu-failure",
            "missing-procedure",
            "shader-failure",
            "link-failure",
            "target-failure",
            "render-failure",
            "readback-failure",
        ],
    )
    def test_gpu_failure_cleanup_and_software_fallback(
        self, harness: ShowcaseHarness, scenario: str
    ) -> None:
        """Release GL objects before the context and surface failed frames to callers."""
        run_process(
            [str(harness.output_executable), scenario, "240", "320"],
            name=f"exercise ARMADA offscreen GPU {scenario}",
            timeout=10,
            check=True,
        )

    @pytest.mark.parametrize(
        "scenario",
        [
            "frame-small-output",
            "frame-wrong-context",
            "frame-pending-error",
            "frame-invalid-dimensions",
        ],
    )
    def test_shared_gpu_frame_rejects_invalid_reads_without_changing_pixels(
        self, harness: ShowcaseHarness, scenario: str
    ) -> None:
        """Link the frame adapter and recover after capacity/context/GL errors."""
        run_process(
            [str(harness.output_executable), scenario, "237", "269"],
            name=f"exercise shared GPU frame {scenario}",
            timeout=10,
            check=True,
        )

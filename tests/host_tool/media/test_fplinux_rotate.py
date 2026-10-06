# SPDX-License-Identifier: GPL-2.0-only
"""Host checks for the independently testable image-rotation contract."""

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

APORT = ROOT / "alpine/aports/fplinux-rotate"
SHARED = ROOT / "include/fplinux"
HARNESS = ROOT / "tests/host_tool/media/fplinux-rotate-core.c"


class FplinuxRotateHostToolTests:
    """Exercise real production objects without claiming V4L2 hardware coverage."""

    core: Path
    executable: Path

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Build one CPU oracle and one CLI for the controlled-file cases."""
        with ExitStack() as cleanup:
            build_directory = tempfile.TemporaryDirectory()
            cleanup.enter_context(build_directory)
            cls.core = Path(build_directory.name) / "fplinux-rotate-core-test"
            cls.executable = Path(build_directory.name) / "fplinux-rotate"
            cls._compile(cls.core, HARNESS, APORT / "fplinux-rotate-core.c")
            cls._compile(
                cls.executable,
                APORT / "fplinux-rotate.c",
                APORT / "fplinux-rotate-core.c",
                APORT / "rotate-rota.c",
                ROOT / "lib/fplinux/fplinux-drm-session.c",
                ROOT / "lib/fplinux/fplinux-cli.c",
            )
            yield

    @staticmethod
    def _compile(output: Path, *sources: Path) -> None:
        drm_flags = shlex.split(
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
                "-O2",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{APORT}",
                f"-I{SHARED}",
                *(str(source) for source in sources),
                *drm_flags,
                "-o",
                str(output),
            ],
            name="compile FPLinux rotation host executable",
            timeout=30,
            check=True,
        )

    def test_cpu_reference_against_independent_oracle(self) -> None:
        """Protect crop/stride/chroma/guard/source invariants for every mode."""
        executable = self.core
        run_process(
            [str(executable)],
            name="run FPLinux rotation CPU oracle",
            timeout=30,
            check=True,
        )

    def test_cpu_cli_rotates_nv12_luma_and_chroma_bytes(self) -> None:
        """A 4x2 NV12 image keeps both planes in its 2x4 rotated raw output."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = self.executable
            source = directory / "source.raw"
            output = directory / "result.raw"
            source.write_bytes(bytes([1, 2, 3, 4, 5, 6, 7, 8, 91, 92, 93, 94]))
            result = run_process(
                [
                    str(executable),
                    "--engine",
                    "cpu",
                    "--format",
                    "nv12",
                    "--width",
                    "4",
                    "--height",
                    "2",
                    "--rotate",
                    "90",
                    "--input",
                    str(source),
                    "--output",
                    str(output),
                ],
                name="run FPLinux NV12 rotation CPU CLI",
                timeout=30,
                check=False,
            )
            assert (result.returncode) == (0), result.stderr
            assert (output.read_bytes()) == (bytes([5, 1, 6, 2, 7, 3, 8, 4, 91, 92, 93, 94]))

    def test_cpu_cli_rotates_generated_image_and_reports_measurement(self) -> None:
        """Without --input, a 90-degree CPU rotation swaps the generated image's dimensions."""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = self.executable
            output = directory / "result.raw"
            result = run_process(
                [
                    str(executable),
                    "--engine",
                    "cpu",
                    "--format",
                    "rgb565",
                    "--width",
                    "320",
                    "--height",
                    "240",
                    "--rotate",
                    "90",
                    "--iterations",
                    "2",
                    "--output",
                    str(output),
                ],
                name="run FPLinux rotation CPU CLI",
                timeout=30,
                check=False,
            )
            assert (result.returncode) == (0), result.stderr
            assert ("benchmark stage=selected engine=cpu iterations=2") in (result.stdout)
            assert ("process_user_us=") in (result.stdout)
            assert ("result format=rgb565 width=240 height=320 ") in (result.stdout)
            assert (output.stat().st_size) == (240 * 320 * 2)

# SPDX-License-Identifier: GPL-2.0-only
"""Link the GPU output with controlled EGL, GL and display boundaries."""

from __future__ import annotations

import shlex
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def gpu_output_harness(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Exercise the actual output object without opening a real GPU or display."""
    with c_build_directory(tmp_path_factory, "gpu-output") as temporary:
        executable = temporary / "gpu-output"
        flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read GPU output header flags",
                timeout=10,
                check=True,
            ).stdout
        )
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-isystem",
                "/opt/quality/include",
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/gpu/gpu-output.c"),
                str(ROOT / "lib/fplinux/fplinux-gpu-output.c"),
                "-Wl,--wrap=ioctl,--wrap=close",
                *flags,
            ],
            name="compile GPU output host harness",
            check=True,
        )
        yield executable


def test_gpu_output_state_transport_failures_and_cleanup(gpu_output_harness: Path) -> None:
    """Check ownership and frame safety without claiming shader pixels or hardware."""
    run_process(
        [str(gpu_output_harness)],
        name="exercise GPU output host contracts",
        timeout=10,
        check=True,
    )

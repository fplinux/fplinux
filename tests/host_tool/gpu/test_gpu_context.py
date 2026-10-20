# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the GPU context with synthetic EGL/DRM boundaries."""

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
def gpu_context_harness(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the production object without an EGL or GLES runtime dependency."""
    with c_build_directory(tmp_path_factory, "gpu-context") as temporary:
        executable = temporary / "gpu-context"
        flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read GPU context header flags",
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
                str(ROOT / "tests/host_tool/gpu/gpu-context.c"),
                str(ROOT / "lib/fplinux/fplinux-gpu.c"),
                "-Wl,--wrap=dlopen,--wrap=dlsym,--wrap=dlclose,--wrap=dlerror",
                "-Wl,--wrap=open,--wrap=close,--wrap=fstat,--wrap=stat",
                "-Wl,--wrap=drmGetNodeTypeFromFd,--wrap=drmGetVersion",
                "-Wl,--wrap=drmFreeVersion,--wrap=drmGetDevices2,--wrap=drmFreeDevices",
                *flags,
                "-ldl",
            ],
            name="compile GPU context host harness",
            check=True,
        )
        yield executable


@pytest.mark.parametrize(
    "scenario",
    [
        "gles",
        "desktop",
        "automatic",
        "ambiguous",
        "missing-provider",
        "missing-procedure",
        "wrong-device",
        "software-device",
        "software-renderer",
        "desktop-core",
        "pbuffer-width-limit",
        "pbuffer-height-limit",
        "surface-failure",
        "make-current-failure",
        "invalid-dimensions",
    ],
)
def test_gpu_context_admission_and_cleanup(gpu_context_harness: Path, scenario: str) -> None:
    """Check the public context contract without claiming a real GPU result."""
    run_process(
        [str(gpu_context_harness), scenario],
        name=f"exercise GPU context {scenario}",
        timeout=10,
        check=True,
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Keep an exported display buffer alive through the borrower's lifetime."""

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
def drm_buffer_borrow(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the session object with controlled export and teardown calls."""
    with c_build_directory(tmp_path_factory, "drm-buffer-borrow") as temporary:
        executable = temporary / "drm-buffer-borrow"
        flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "libdrm"],
                name="read DRM buffer borrow header flags",
                timeout=10,
                check=True,
            ).stdout
        )
        compile_c(
            executable,
            [
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/display/fplinux-drm-buffer-borrow.c"),
                str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                "-Wl,--wrap=drmPrimeHandleToFD,--wrap=drmModeRmFB",
                "-Wl,--wrap=drmModeDestroyPropertyBlob,--wrap=drmIoctl,--wrap=munmap",
                *flags,
                "-ldrm",
            ],
            name="compile DRM buffer borrow host harness",
            check=True,
        )
        yield executable


def test_exported_buffer_borrow_blocks_session_teardown(drm_buffer_borrow: Path) -> None:
    """A premature close preserves mapping, descriptor and the active callback."""
    run_process(
        [str(drm_buffer_borrow)],
        name="exercise exclusive DRM buffer borrow and close guard",
        timeout=10,
        check=True,
    )

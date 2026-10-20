# SPDX-License-Identifier: GPL-2.0-only
"""Exercise presentation/close with synthetic selected DRM objects, without VT acquisition."""

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
def drm_properties(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the session TU with controlled property, atomic and fence boundaries."""
    with c_build_directory(tmp_path_factory, "drm-properties") as temporary:
        executable = temporary / "drm-properties"
        flags = shlex.split(
            run_process(
                ["pkg-config", "--cflags", "--libs", "libdrm"],
                name="read DRM property harness compiler flags",
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
                str(ROOT / "tests/host_tool/display/fplinux-drm-properties.c"),
                str(ROOT / "lib/fplinux/fplinux-drm-session.c"),
                "-Wl,--wrap=drmModeObjectGetProperties,--wrap=drmModeFreeObjectProperties",
                "-Wl,--wrap=drmModeGetProperty,--wrap=drmModeFreeProperty",
                "-Wl,--wrap=drmModeAtomicAlloc,--wrap=drmModeAtomicAddProperty",
                "-Wl,--wrap=drmModeAtomicCommit,--wrap=drmModeAtomicFree",
                "-Wl,--wrap=drmModeRmFB,--wrap=drmModeDestroyPropertyBlob,--wrap=ioctl",
                "-Wl,--wrap=drmDropMaster",
                *flags,
            ],
            name="compile DRM presentation property host harness",
            check=True,
        )
        yield executable


@pytest.mark.parametrize(
    "mode",
    [
        "cached-frames",
        "close-reset",
        "missing-property",
        "query-error",
        "atomic-add-error",
        "commit-error",
        "fence-error",
        "async-queue",
        "blocking-drain",
        "vt-drain",
        "close-drain",
        "async-fence-error",
        "async-false-fence",
        "async-timeout",
        "async-missing-fence",
    ],
)
def test_presentation_properties_preserve_requests_errors_and_session_lifetime(
    drm_properties: Path, mode: str
) -> None:
    """Close resets IDs before a new synthetic selection; open/VT is not exercised."""
    run_process(
        [str(drm_properties), mode],
        name=f"exercise DRM presentation property behavior: {mode}",
        timeout=10,
        check=True,
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Run the phone screenshot reader with a DRM peer and a real file mapping."""

from __future__ import annotations

import os
import subprocess
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def reader(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "screenshot-reader") as directory:
        executable = directory / "screenshot"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(ROOT / "alpine/aports/fplinux-session/screenshot.c"),
                str(ROOT / "tests/host_tool/display/drm/screenshot-peer.c"),
                "-Wl,--wrap=open,--wrap=open64,--wrap=ioctl",
            ],
            name="compile the phone screenshot reader and DRM peer",
        )
        yield executable


@pytest.mark.parametrize(
    ("scenario", "error"),
    [
        ("linear", b""),
        ("unsupported", b"expected a linear RGB565 frame"),
        ("page-flip", b"DRM scanout changed during capture"),
    ],
)
def test_reader_converts_the_selected_frame_or_returns_a_diagnostic(
    reader: Path, tmp_path: Path, scenario: str, error: bytes
) -> None:
    # The first two bytes and end of the first row are padding.
    (tmp_path / "pixels").write_bytes(b"12\x00\xf8\xe0\x07xx\x1f\x00\xff\xff")
    result = subprocess.run(
        [reader],
        cwd=tmp_path,
        env={**os.environ, "SCANOUT_SCENARIO": scenario},
        capture_output=True,
        timeout=5,
        check=False,
    )

    if error:
        assert result.returncode == 1
        assert result.stdout == b""
        assert error in result.stderr
    else:
        assert result.returncode == 0, result.stderr
        assert result.stdout == b"P6\n2 2\n255\n\xff\0\0\0\xff\0\0\0\xff\xff\xff\xff"

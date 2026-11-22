# SPDX-License-Identifier: GPL-2.0-only
"""Files preview output, limits and cancellation through the native worker."""

from __future__ import annotations

import shutil
import struct
import zlib
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def preview_worker(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    aport = ROOT / "alpine/aports/fplinux-files"
    with c_build_directory(tmp_path_factory, "files-preview") as directory:
        executable = directory / "preview"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(aport),
                str(ROOT / "tests/host_tool/files/preview.c"),
                str(aport / "files-preview.c"),
            ],
            name="compile Files preview production object",
        )
        yield executable


@pytest.fixture
def waiting_helper(tmp_path: Path) -> Path:
    helper = tmp_path / "slow helper"
    helper.write_text("#!/bin/sh\nexec sleep 8\n", encoding="utf-8")
    helper.chmod(0o700)
    return helper


def png_chunk(tag: bytes, contents: bytes) -> bytes:
    return (
        struct.pack(">I", len(contents))
        + tag
        + contents
        + struct.pack(">I", zlib.crc32(tag + contents))
    )


def png(width: int, height: int, *, total_bytes: int = 0) -> bytes:
    header = struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)
    rows = (b"\x00" + b"\xc0\x30\x05" * width) * height
    image = (
        b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(rows))
    )
    end = png_chunk(b"IEND", b"")
    if total_bytes:
        keyword = b"Description\x00"
        padding = total_bytes - len(image) - len(end) - 12 - len(keyword)
        image += png_chunk(b"tEXt", keyword + b"x" * padding)
    return image + end


def helper_program(name: str) -> str:
    executable = shutil.which(name)
    assert executable and executable.startswith("/")
    return executable


@pytest.mark.parametrize(
    ("dimensions", "total_bytes", "expected_dimensions"),
    [
        ((128, 1), 65536, (128, 1)),
        ((129, 1), 0, (0, 0)),
        ((1, 129), 0, (0, 0)),
        ((1, 1), 65537, (0, 0)),
    ],
    ids=["at-limits", "width", "height", "output-size"],
)
def test_preview_validates_and_caches_helper_output(
    preview_worker: Path,
    tmp_path: Path,
    dimensions: tuple[int, int],
    total_bytes: int,
    expected_dimensions: tuple[int, int],
) -> None:
    source = tmp_path / "file ' with spaces.png"
    source.write_bytes(png(*dimensions, total_bytes=total_bytes))
    expected_width, expected_height = expected_dimensions
    run_process(
        [
            str(preview_worker),
            "preview",
            helper_program("cat"),
            str(source),
            str(expected_width),
            str(expected_height),
        ],
        name="validate the PNG preview contract",
        timeout=10,
        check=True,
    )


def test_preview_times_out_and_caches_unavailable(
    preview_worker: Path, waiting_helper: Path, tmp_path: Path
) -> None:
    source = tmp_path / "file.png"
    source.write_bytes(png(2, 1))
    run_process(
        [str(preview_worker), "preview", str(waiting_helper), str(source), "0", "0"],
        name="time out the active preview helper",
        timeout=10,
        check=True,
    )


def test_preview_nonzero_exit_remains_unavailable_with_png_output(
    preview_worker: Path, tmp_path: Path
) -> None:
    helper = tmp_path / "unavailable helper"
    helper.write_text('#!/bin/sh\ncat "$1"\nexit 1\n', encoding="utf-8")
    helper.chmod(0o700)
    source = tmp_path / "file.png"
    source.write_bytes(png(2, 1))
    run_process(
        [str(preview_worker), "preview", str(helper), str(source), "0", "0"],
        name="reject a PNG from an unavailable preview helper",
        timeout=10,
        check=True,
    )


def test_preview_reset_discards_running_and_queued_requests(
    preview_worker: Path, waiting_helper: Path, tmp_path: Path
) -> None:
    source = tmp_path / "file.png"
    source.write_bytes(png(2, 1))
    run_process(
        [
            str(preview_worker),
            "cancel",
            str(waiting_helper),
            helper_program("cat"),
            str(source),
        ],
        name="reset active preview and reuse the directory index",
        timeout=10,
        check=True,
    )

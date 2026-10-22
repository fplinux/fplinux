# SPDX-License-Identifier: GPL-2.0-only
"""Owned image pixels decoded and drawn with production code and real LVGL."""

from __future__ import annotations

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
def ui_image_decode(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Iterator[Path]:
    """Link the image API as a separate object, retaining native decoder and canvas."""
    headers, archive = lvgl_library
    with c_build_directory(tmp_path_factory, "ui-image-decode") as directory:
        executable = directory / "ui-image-decode"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-isystem",
                str(headers),
                "-DLV_KCONFIG_IGNORE",
                "-DLV_LVGL_H_INCLUDE_SYSTEM",
                f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/gui/fplinux-ui-image-decode.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-screen.c"),
                str(archive),
                "-lm",
            ],
            name="link the production image loader with native LVGL decoders",
        )
        yield executable


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def rgba_png(width: int, height: int, pixels: bytes) -> bytes:
    """Construct test-owned RGBA PNG input with unfiltered scanlines."""
    assert len(pixels) == width * height * 4
    stride = width * 4
    rows = b"".join(b"\0" + pixels[y * stride : (y + 1) * stride] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(rows))
        + png_chunk(b"IEND", b"")
    )


def run_image_case(executable: Path, scenario: str, source: Path | None = None) -> None:
    arguments = [str(executable), scenario]
    if source is not None:
        arguments.append(str(source))
    result = run_process(
        arguments,
        name=f"native image API: {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"{scenario}: ok\n")


@pytest.mark.parametrize("symbolic", [False, True], ids=["rgba", "symbolic"])
def test_fully_transparent_png_is_a_valid_owned_image(
    ui_image_decode: Path, tmp_path: Path, *, symbolic: bool
) -> None:
    source = tmp_path / ("clear-symbolic.png" if symbolic else "clear.png")
    source.write_bytes(rgba_png(2, 2, bytes([255, 80, 30, 0]) * 4))
    scenario = "transparent-symbolic" if symbolic else "transparent-png"
    run_image_case(ui_image_decode, scenario, source)


@pytest.mark.parametrize("scenario", ["transparent-a8", "a8-mask"])
def test_a8_masks_keep_alpha_and_own_their_pixels(ui_image_decode: Path, scenario: str) -> None:
    run_image_case(ui_image_decode, scenario)


def test_valid_png_can_become_fully_transparent_after_downscale(
    ui_image_decode: Path, tmp_path: Path
) -> None:
    source = tmp_path / "small-coverage.png"
    pixels = bytes([255, 0, 0, 255]) + bytes(32 * 32 * 4 - 4)
    source.write_bytes(rgba_png(32, 32, pixels))
    run_image_case(ui_image_decode, "transparent-downscale", source)


def test_area_scaling_keeps_proportions_and_never_enlarges(ui_image_decode: Path) -> None:
    run_image_case(ui_image_decode, "area-scale")


def test_loaded_png_stays_drawable_after_its_file_is_deleted(
    ui_image_decode: Path, tmp_path: Path
) -> None:
    source = tmp_path / "owned.png"
    source.write_bytes(rgba_png(2, 1, bytes([255, 0, 0, 255, 0, 0, 255, 255])))
    run_image_case(ui_image_decode, "owned-file", source)
    assert not source.exists()


def test_symbolic_png_preserves_its_alpha_mask(ui_image_decode: Path, tmp_path: Path) -> None:
    source = tmp_path / "colour-symbolic.png"
    source.write_bytes(rgba_png(2, 1, bytes([255, 0, 0, 255, 0, 0, 255, 128])))
    run_image_case(ui_image_decode, "symbolic-mask", source)


@pytest.mark.parametrize("malformation", ["bad-signature", "bad-idat", "truncated"])
def test_invalid_png_returns_einval(
    ui_image_decode: Path, tmp_path: Path, malformation: str
) -> None:
    source = tmp_path / "broken.png"
    image = rgba_png(2, 1, bytes([255, 0, 0, 255, 0, 0, 255, 255]))
    if malformation == "bad-signature":
        image = b"invalid!" + image[8:]
    elif malformation == "bad-idat":
        image = image[:33] + png_chunk(b"IDAT", b"invalid zlib") + png_chunk(b"IEND", b"")
    else:
        image = image[:40]
    source.write_bytes(image)
    run_image_case(ui_image_decode, "invalid-png", source)


def test_png_file_size_limit_returns_efbig(ui_image_decode: Path, tmp_path: Path) -> None:
    source = tmp_path / "large.png"
    with source.open("wb") as stream:
        stream.truncate(1_048_577)
    run_image_case(ui_image_decode, "file-limit", source)


def test_decoded_pixel_limit_returns_efbig(ui_image_decode: Path, tmp_path: Path) -> None:
    source = tmp_path / "many-pixels.png"
    source.write_bytes(rgba_png(1025, 1024, bytes(1025 * 1024 * 4)))
    run_image_case(ui_image_decode, "pixel-limit", source)


def test_image_invalid_arguments_return_einval(ui_image_decode: Path) -> None:
    run_image_case(ui_image_decode, "invalid-arguments")

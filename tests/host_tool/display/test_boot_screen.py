# SPDX-License-Identifier: GPL-2.0-only
"""The real bootstrap state adapter draws a static or failed frame."""

from __future__ import annotations

import json
import os
import struct
import sys
from typing import TYPE_CHECKING

import fplinux_ui_profile
import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.host_tool.display.status_resources import generate
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module", params=["128x128", "128x160", "176x220", "240x320", "320x240"])
def boot_screen(
    request: pytest.FixtureRequest, tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[tuple[str, Path]]:
    profile = str(request.param)
    with c_build_directory(tmp_path_factory, f"boot-screen-{profile}") as directory:
        fonts = generate(directory, ui_host_inputs, profile)
        tokens = json.loads((ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text())
        generated = directory / "generated"
        generated.mkdir()
        (generated / "fplinux-status-profile.h").write_text(
            fplinux_ui_profile.render_status(tokens, profile)
        )
        run_process(
            [
                sys.executable,
                str(ROOT / "alpine/aports/fplinux-brand/generate-brand.py"),
                "--profile",
                str(ROOT / "lib/fplinux/fplinux-ui-profile.json"),
                "--boot-out",
                str(directory),
                "--screen",
                profile,
            ],
            name=f"generate bootstrap brand for {profile}",
            timeout=60,
            env=os.environ
            | {
                "PYTHONPATH": str(ROOT / "alpine/aports/fplinux-icons"),
                "NODE_PATH": "/opt/quality/node-tools/node_modules",
            },
            check=True,
        )
        executable = directory / "boot-screen"
        compile_c(
            executable,
            [
                "-std=c99",
                "-pedantic",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{directory}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{ROOT / 'bootstrap/fplinux-boot-screen'}",
                str(ROOT / "tests/host_tool/display/fplinux-boot-screen.c"),
                str(ROOT / "bootstrap/fplinux-boot-screen/boot-screen.c"),
                str(ROOT / "lib/fplinux/fplinux-status-screen.c"),
                str(fonts),
            ],
            name="compile selected bootstrap status screen",
        )
        yield profile, executable


def test_boot_and_failure_are_visible(boot_screen: tuple[str, Path], tmp_path: Path) -> None:
    profile, executable = boot_screen
    width, height = map(int, profile.split("x"))
    images = []
    for state in ("boot", "expected", "error", "error2"):
        output = tmp_path / state
        run_process(
            [str(executable), str(width), str(height), state, str(output)],
            name=f"render bootstrap {state} on {profile}",
            timeout=10,
            check=True,
        )
        images.append(struct.unpack(f"={width * height}H", output.read_bytes()))
    boot, expected, error, other_error = images
    assert boot == expected
    assert boot != error
    assert error != other_error
    assert 0xE71B in boot  # Light centre of the shared boot mark.
    mark_size = {"128x128": 48, "128x160": 64, "176x220": 80, "240x320": 96, "320x240": 96}[
        profile
    ]
    mark_top = {"128x128": 27, "128x160": 32, "176x220": 48, "240x320": 80, "320x240": 48}[profile]
    mark_pixels = [(i % width, i // width) for i, pixel in enumerate(boot) if pixel == 0xE71B]
    assert all(mark_top <= y < mark_top + mark_size for _, y in mark_pixels)
    assert any(pixel != 0x1082 for pixel in boot[width * (mark_top + mark_size) :])
    assert all(pixel == 0x1082 for pixel in boot[: width * (mark_top - 1)])
    assert 0xFDB5 in error  # Dark theme danger, without a full-width error bar.
    assert (
        max(error[y * width : (y + 1) * width].count(0xFDB5) for y in range(height)) < width // 2
    )


@pytest.mark.parametrize(
    "message",
    [
        "MICROSD NOT FOUND OR UNREADABLE. POWER OFF AND CHECK THE CARD",
        "FPLINUX.ITB NOT FOUND OR UNREADABLE. REWRITE THE BOOT CARD",
    ],
    ids=["card-error", "image-error"],
)
def test_boot_instructions_fit_and_remain_visible(
    boot_screen: tuple[str, Path], tmp_path: Path, message: str
) -> None:
    profile, executable = boot_screen
    width, height = map(int, profile.split("x"))
    margin = {"128x128": 4, "128x160": 4, "176x220": 6, "240x320": 8, "320x240": 6}[profile]

    def render(detail: str) -> tuple[int, ...]:
        output = tmp_path / "frame"
        run_process(
            [
                str(executable),
                str(width),
                str(height),
                "error",
                str(output),
                detail,
            ],
            name=f"render error instruction on {profile}",
            timeout=10,
            check=True,
        )
        return struct.unpack(f"={width * height}H", output.read_bytes())

    # Each successive word must add visible pixels in the instruction area.
    previous = render("")
    words = message.split()
    for count, word in enumerate(words, start=1):
        current = render(" ".join(words[:count]))
        added = [
            (index % width, index // width)
            for index, (before, after) in enumerate(zip(previous, current, strict=True))
            if before == 0x1082 and after != 0x1082
        ]
        assert added, f"{profile}: {word!r} has no visible pixels"
        assert all(
            margin <= x < width - margin and margin <= y < height - margin for x, y in added
        )
        previous = current

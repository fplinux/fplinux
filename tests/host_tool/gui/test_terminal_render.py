# SPDX-License-Identifier: GPL-2.0-only
"""Terminal drawing geometry through a controlled LVGL drawing boundary."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.fixtures.psf_font import write_psf_font, write_solid_ascii_font
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def terminal_renderer(
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[tuple[Path, dict[str, Path]]]:
    with c_build_directory(tmp_path_factory, "terminal-render") as directory:
        executable = directory / "terminal-render"
        compact = directory / "compact.psf"
        write_psf_font(
            compact,
            width=6,
            height=12,
            glyphs={
                0x20: bytes(12),
                ord("W"): bytes.fromhex("00 84 84 84 84 84 a8 a8 d8 84 00 00"),
                ord("Ш"): bytes.fromhex("84 48 30 fc 00 00 00 00 00 00 00 00"),
                ord("_"): bytes.fromhex("00 00 00 00 00 00 00 00 00 fc 00 00"),
                ord("a"): bytes.fromhex("00 00 00 70 08 78 88 88 78 00 00 00"),
                ord("Z"): bytes.fromhex("00 fc 04 08 10 20 40 80 fc 00 00 00"),
                ord("Q"): bytes.fromhex("00 78 84 84 84 84 a4 98 7c 00 00 00"),
                0xFFFD: bytes.fromhex("fc 84 b4 a4 a4 94 84 94 84 fc 00 00"),
            },
        )
        larger = directory / "larger.psf"
        write_psf_font(
            larger,
            width=8,
            height=16,
            glyphs={
                0x20: bytes(16),
                ord("W"): bytes.fromhex("00 81 81 81 81 81 a5 a5 a5 db 81 00 00 00 00 00"),
                ord("Ш"): bytes.fromhex("81 42 24 ff 00 00 00 00 00 00 00 00 00 00 00 00"),
                ord("_"): bytes.fromhex("00 00 00 00 00 00 00 00 00 00 00 ff 00 00 00 00"),
                ord("a"): bytes.fromhex("00 00 00 00 3c 02 3e 42 42 3e 00 00 00 00 00 00"),
                ord("Z"): bytes.fromhex("00 ff 01 02 04 08 10 20 40 80 ff 00 00 00 00 00"),
                ord("Q"): bytes.fromhex("00 7e 81 81 81 81 81 89 85 7e 01 00 00 00 00 00"),
                0xFFFD: bytes.fromhex("ff 81 bd a5 a5 a5 89 91 81 91 81 ff 00 00 00 00"),
            },
        )
        aport = ROOT / "alpine/aports/fplinux-terminal"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tests/host_tool/gui/terminal-boundary'}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{aport}",
                str(ROOT / "tests/host_tool/gui/terminal-render-test.c"),
                str(aport / "terminal-render.c"),
                str(aport / "terminal-engine.c"),
                str(aport / "terminal-input.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                str(ROOT / "lib/fplinux/fplinux-text-input.c"),
                str(ROOT / "lib/fplinux/fplinux-language.c"),
                str(ROOT / "lib/fplinux/fplinux-font.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                "-ltsm",
                *gettext_link_flags(),
            ],
            name="link terminal renderer with a drawing recorder",
        )
        yield executable, {"6x12": compact, "8x16": larger}


@pytest.mark.parametrize(
    "scenario",
    [
        "font",
        "glyphs",
        "decorations",
        "preedit",
        "hidden-cursor-underline",
        "blinking-cursor-underline",
        "layouts",
        "resize",
        "resize-alternate",
    ],
)
@pytest.mark.parametrize("size", ["6x12", "8x16"])
def test_terminal_draws_psf_glyphs_inside_the_selected_cells(
    terminal_renderer: tuple[Path, dict[str, Path]], scenario: str, size: str
) -> None:
    executable, fonts = terminal_renderer
    result = run_process(
        [str(executable), scenario, str(fonts[size]), size],
        name=f"record terminal {scenario} drawing geometry with {size} cells",
        timeout=10,
        check=True,
    )
    assert result.stdout == f"terminal-render {scenario}: OK\n"


@pytest.mark.parametrize(("width", "height"), [(8, 12), (6, 16), (6, 10)])
def test_terminal_grid_rejects_other_font_dimensions(
    terminal_renderer: tuple[Path, dict[str, Path]], tmp_path: Path, width: int, height: int
) -> None:
    executable, _ = terminal_renderer
    font = tmp_path / "other-size.psf"
    write_solid_ascii_font(font, width=width, height=height)
    result = run_process(
        [str(executable), "reject-font", str(font), "6x12"],
        name="reject a terminal font with incompatible cell dimensions",
        timeout=10,
        check=True,
    )
    assert result.stdout == "terminal-render reject-font: OK\n"

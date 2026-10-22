# SPDX-License-Identifier: GPL-2.0-only
"""Compile header consumers with the source analyzer's dependency configuration."""

from __future__ import annotations

from pathlib import Path

import pytest
from fplinux_cli.common import ROOT
from fplinux_cli.quality import source as source_check

from tests.process import run_process

FIXTURES = Path(__file__).parent / "fixtures/source-analysis"


def compile_fixture(fixture: str, flags: list[str], output: Path) -> None:
    """Check C declarations without linking any GUI runtime or device backend."""
    result = run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            *flags,
            "-x",
            "c",
            "-c",
            str(FIXTURES / fixture),
            "-o",
            str(output),
        ],
        name=f"source analysis headers: {fixture}",
        timeout=30,
        cwd=ROOT,
    )
    assert result.returncode == 0, result.stderr
    assert output.stat().st_size > 0


@pytest.mark.parametrize(
    "source",
    ["lib/fplinux/fplinux-ui-text.c", "alpine/aports/fplinux-notes/fplinux-notes.c"],
    ids=["shared-runtime", "application"],
)
def test_ui_header_consumers_compile_with_rgb565_configuration(
    source: str, tmp_path: Path
) -> None:
    """Runtime and application headers use the same configured LVGL declarations."""
    flags = source_check.userspace_c_include_flags(source)
    compile_fixture("ui.c.fixture", flags, tmp_path / "ui.o")


def test_compositor_header_consumer_compiles_with_input_and_pixel_dependencies(
    tmp_path: Path,
) -> None:
    """Weston public declarations remain usable by the session shell analyzer."""
    flags = source_check.userspace_c_include_flags("alpine/aports/fplinux-session/fplinux-shell.c")
    compile_fixture("compositor.c.fixture", flags, tmp_path / "compositor.o")


def test_clock_presentation_headers_compile_with_ui_dependencies(
    tmp_path: Path,
) -> None:
    """Fixed-offset presentation declarations use the configured GUI headers."""
    flags = source_check.userspace_c_include_flags(
        "alpine/aports/fplinux-clocks/clock-presentation.c"
    )
    flags.extend(("-I", str(ROOT / "alpine/aports/fplinux-clocks")))
    compile_fixture("clock.c.fixture", flags, tmp_path / "clock.o")


def test_generated_xdg_shell_header_compiles_with_wayland_client(tmp_path: Path) -> None:
    """The installed protocol XML and scanner produce usable client declarations."""
    source_check.generate_ui_protocol_header(tmp_path / "xdg-shell-client-protocol.h")
    flags = [*source_check.pkg_config_cflags("wayland-client"), "-I", str(tmp_path)]
    compile_fixture("protocol.c.fixture", flags, tmp_path / "protocol.o")

# SPDX-License-Identifier: GPL-2.0-only
"""Actual client SHM commits and acknowledgements with a minimal compositor peer."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture(scope="module")
def client_frames(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Path:
    """Link the actual UI library and renderer against real Wayland libraries."""
    directory = tmp_path_factory.mktemp("ui-client-frames")
    protocol = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    for kind, filename in (
        ("client-header", "xdg-shell-client-protocol.h"),
        ("server-header", "xdg-shell-server-protocol.h"),
        ("private-code", "xdg-shell-protocol.c"),
    ):
        run_process(
            ["wayland-scanner", kind, protocol, str(directory / filename)],
            name=f"generate the test xdg-shell {kind}",
            timeout=10,
            check=True,
        )
    headers, archive = lvgl_library
    flags = run_process(
        [
            "pkg-config",
            "--cflags",
            "--libs",
            "wayland-client",
            "wayland-server",
            "xkbcommon",
            "json-c",
        ],
        name="resolve the actual client and compositor libraries",
        timeout=10,
        check=True,
    ).stdout.split()
    executable = directory / "client-frames"
    sources = [
        "fplinux-ui.c",
        "fplinux-ui-session.c",
        "fplinux-ui-text.c",
        "fplinux-ui-text-model.c",
        "fplinux-ui-menu.c",
        "fplinux-ui-screen.c",
        "fplinux-ui-screen-model.c",
        "fplinux-ui-listview.c",
        "fplinux-ui-locale.c",
        "fplinux-ui-profile.c",
        "fplinux-ui-softkey-layout.c",
        "fplinux-multitap.c",
        "fplinux-language.c",
    ]
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-isystem",
            str(headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{directory}",
            str(ROOT / "tests/host_tool/gui/ui-client-frames.c"),
            *(str(ROOT / "lib/fplinux" / source) for source in sources),
            str(directory / "xdg-shell-protocol.c"),
            str(archive),
            *flags,
            *gettext_link_flags(),
            "-lm",
        ],
        name="link the complete UI client and test compositor",
    )
    return executable


def test_successful_exit_retains_frame_until_manager_close(
    client_frames: Path, tmp_path: Path
) -> None:
    """A successful Back retains visible pixels and waits for manager CLOSE."""
    result = run_process(
        [str(client_frames), "exit", str(tmp_path / "session.sock")],
        name="retain the last client SHM frame while exiting",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("exit: ok\n")


def test_failed_exit_send_closes_client_without_waiting_for_manager(
    client_frames: Path, tmp_path: Path
) -> None:
    """A lost manager connection makes Back fail and closes the client promptly."""
    result = run_process(
        [str(client_frames), "disconnect", str(tmp_path / "session.sock")],
        name="close the client when the manager disappears during Back",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("disconnect: ok\n")


def test_canvas_softkeys_keep_full_bar_alignment_without_redundant_frame(
    client_frames: Path, tmp_path: Path
) -> None:
    """The launcher-style real LVGL bar centres labels and leaves identical updates quiet."""
    result = run_process(
        [str(client_frames), "softkeys", str(tmp_path / "session.sock")],
        name="align Canvas legends and avoid a redundant client SHM frame",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("softkeys: ok\n")


@pytest.mark.parametrize("scenario", ["active", "setting", "locale", "error"])
def test_acknowledgement_waits_for_complete_new_frame(
    client_frames: Path, tmp_path: Path, scenario: str
) -> None:
    """Busy SHM buffers defer ACK until a complete callback redraw is committed."""
    result = run_process(
        [str(client_frames), scenario, str(tmp_path / "session.sock")],
        name=f"acknowledge the fully committed client {scenario} frame",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"{scenario}: ok\n")

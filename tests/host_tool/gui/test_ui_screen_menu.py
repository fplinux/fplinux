# SPDX-License-Identifier: GPL-2.0-only
"""Linked screen commands with LVGL, list-view and runtime stand-ins.

The stand-ins retain geometry, row selection and menu choices; the real editing
model checks that hidden fields stop held input. This does not execute runtime
transport or render pixels.
"""

from pathlib import Path

from tests.gettext_support import gettext_link_flags
from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


def test_screen_commands_follow_their_items(tmp_path: Path) -> None:
    """Options use the current row index and close when their target disappears."""
    executable = tmp_path / "screen-menu"
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            "-isystem",
            "/opt/quality/include",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            str(ROOT / "tests/host_tool/gui/fplinux-ui-screen-menu.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-screen.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-screen-model.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-text-model.c"),
            str(ROOT / "lib/fplinux/fplinux-multitap.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            *gettext_link_flags(),
            "-o",
            str(executable),
        ],
        name="compile screen command component",
        timeout=60,
        check=True,
    )
    run_process(
        [str(executable), str(ROOT / "tests/host_tool/input/fixtures/input-languages")],
        name="run screen commands and hidden text editing",
        timeout=10,
        check=True,
    )

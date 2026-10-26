# SPDX-License-Identifier: GPL-2.0-only
"""Linked screen commands with LVGL, list-view and runtime stand-ins.

The stand-ins retain geometry, text, title scroll state, row selection and menu
choices; the real editing model checks that hidden fields stop held input. Text
advances are controlled test inputs. Scrollbar appearance belongs to the native
LVGL tests. This does not prove typography, execute runtime transport or render pixels.
"""

from pathlib import Path

import pytest

from tests.gettext_support import gettext_link_flags
from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def screen_menu_consumer(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Link actual command allocation with controlled transport and image resources."""
    executable = tmp_path_factory.mktemp("screen-menu") / "screen-menu"
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
            str(ROOT / "lib/fplinux/fplinux-text-input.c"),
            str(ROOT / "lib/fplinux/fplinux-multitap.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            *gettext_link_flags(),
            "-Wl,--wrap=strdup",
            "-o",
            str(executable),
        ],
        name="compile screen command component",
        timeout=60,
        check=True,
    )
    return executable


def test_screen_commands_follow_their_items(screen_menu_consumer: Path) -> None:
    """Commands follow items; header status keeps its geometry and screen owner."""
    run_process(
        [str(screen_menu_consumer), str(ROOT / "tests/host_tool/input/fixtures/input-languages")],
        name="run screen commands and hidden text editing",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("scenario", ["back-last", "synthetic-clear", "no-invented-back"])
def test_options_orders_only_real_back_last_and_preserves_command_semantics(
    screen_menu_consumer: Path, scenario: str
) -> None:
    """Visible actions retain IDs; unavailable glyphs do not remove working commands."""
    run_process(
        [
            str(screen_menu_consumer),
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            scenario,
        ],
        name=f"native command allocation and callbacks: {scenario}",
        timeout=10,
        check=True,
    )

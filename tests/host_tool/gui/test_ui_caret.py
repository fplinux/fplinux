# SPDX-License-Identifier: GPL-2.0-only
"""Native LVGL caret pixels and text-field lifecycle with a controlled clock."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import TYPE_CHECKING

import fplinux_ui_fonts
import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.gettext_support import gettext_link_flags
from tests.host_tool.gui.font_support import synthetic_face
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture(scope="module")
def caret_consumer(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Path:
    """Link the production widgets and editing model with the real renderer."""
    directory = tmp_path_factory.mktemp("ui-caret")
    face = directory / "caret.ttf"
    latin = " /0123456789AHVxFirstSecondThirdFourthFifthSixthLastRead onlyWSpacerAbcToij"
    codes = tuple(sorted({*map(ord, latin), 0x0410, 0x0422, 0x0430, 0x0440, 0x044F}))
    face.write_bytes(synthetic_face(codes))
    spec = fplinux_ui_fonts.FontSpec(
        "caret-14",
        face.name,
        14,
        digits=False,
        ranges=((face.name, codes),),
    )
    with pytest.MonkeyPatch.context() as monkeypatch:
        monkeypatch.setattr(
            fplinux_ui_fonts,
            "SOURCES",
            {
                face.name: fplinux_ui_fonts.Source(
                    "https://example.invalid/caret.ttf",
                    hashlib.sha256(face.read_bytes()).hexdigest(),
                    "controlled test face",
                )
            },
        )
        fplinux_ui_fonts.build(
            [spec], directory, directory, "/opt/quality/node-tools/node_modules/.bin/lv_font_conv"
        )
    headers, archive = lvgl_library
    executable = directory / "caret-consumer"
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
            "-Wl,--wrap=fplinux_multitap_load_languages",
            "-isystem",
            str(headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "tests/host_tool/gui/fplinux-ui-caret.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-text.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-style.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-text-model.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-symbol-picker.c"),
            str(ROOT / "lib/fplinux/fplinux-text-input.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-screen.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-screen-model.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-listview.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-menu.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-locale.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            str(ROOT / "lib/fplinux/fplinux-multitap.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            str(directory / spec.c_name),
            str(directory / "fplinux-ui-fonts.c"),
            str(archive),
            *gettext_link_flags(),
            "-lm",
        ],
        name="link production text and screen widgets with LVGL",
    )
    return executable


@pytest.mark.parametrize(
    "scenario",
    [
        "blink",
        "transitions",
        "scroll",
        "vertical-scroll",
        "screen",
        "deadlines",
        "column",
        "spacing",
    ],
)
def test_native_caret_pixels_and_lifecycle(caret_consumer: Path, scenario: str) -> None:
    """Host-rendered caret phases preserve editing, scrolling and screen focus."""
    result = run_process(
        [
            str(caret_consumer),
            scenario,
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"render native caret: {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"{scenario}: ok\n")


def test_native_form_pages_keep_focus_in_visible_content(caret_consumer: Path) -> None:
    """Read-only forms page with line overlap; a choice scrolled away loses focus."""
    run_process(
        [
            str(caret_consumer),
            "form-pages",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="page the native form and blur a hidden choice",
        timeout=10,
        check=True,
    )


def test_native_menu_restore_reveals_command_after_reordering(caret_consumer: Path) -> None:
    """An unchanged menu retains scroll; a moved chosen command remains fully visible."""
    run_process(
        [
            str(caret_consumer),
            "menu-position",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="restore native menu focus after command reordering",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("width", "height", "offset"),
    [(128, 128, 1), (128, 160, 1), (176, 220, 2), (240, 320, 3), (320, 240, 2)],
)
def test_native_list_form_menu_and_read_only_text_share_scrollbar_appearance(
    caret_consumer: Path, width: int, height: int, offset: int
) -> None:
    """Actual widgets show the same idle thumb and hide it when their content fits."""
    run_process(
        [
            str(caret_consumer),
            "scrollbars",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            str(width),
            str(height),
            str(offset),
        ],
        name=f"native list, form, menu and read-only text scrollbars at {width}x{height}",
        timeout=10,
        check=True,
    )


def test_alert_delivers_all_application_command_ids(caret_consumer: Path) -> None:
    """An alert leaves the stack, reports its command, then releases its widgets."""
    run_process(
        [
            str(caret_consumer),
            "alert-command-ids",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="deliver alert application command IDs through native widgets",
        timeout=10,
        check=True,
    )


def test_hiding_edited_date_restores_saved_native_label(caret_consumer: Path) -> None:
    """Hiding cancels an unfinished date edit without reporting a value change."""
    run_process(
        [
            str(caret_consumer),
            "hidden-date",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="hide and restore the native edited date item",
        timeout=10,
        check=True,
    )


def test_alert_message_updates_native_label_box_and_scroll(caret_consumer: Path) -> None:
    """Message changes resize the busy box and preserve scroll only for equal text."""
    run_process(
        [
            str(caret_consumer),
            "alert-text",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="update the native busy alert message and box",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("operation", ["append", "insert"])
def test_exclusive_first_row_and_refill_keep_one_mark(
    caret_consumer: Path, operation: str
) -> None:
    """Stored exclusive lists acquire one mark without synthetic change events."""
    run_process(
        [
            str(caret_consumer),
            f"exclusive-{operation}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"fill a native exclusive list by {operation}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("kind", ["exclusive", "multiple", "popup"])
def test_form_choices_own_input_text_and_preserve_selection(
    caret_consumer: Path, kind: str
) -> None:
    """Native choices retain mutable input text through selection and deletion."""
    run_process(
        [
            str(caret_consumer),
            f"choice-{kind}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"retain text and selection in a native {kind} form choice",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("editing", ["pending", "clear"])
def test_options_keeps_text_box_suspended_across_visibility(
    caret_consumer: Path, editing: str
) -> None:
    """Real menu suspension stops held editing and the caret until dismissal."""
    run_process(
        [
            str(caret_consumer),
            f"menu-{editing}-visibility",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"restore a native text box under Options after held {editing}",
        timeout=10,
        check=True,
    )


def test_menu_restores_only_focused_canvas_field_after_visibility(
    caret_consumer: Path,
) -> None:
    """Canvas fields keep their own focus choices when a real menu dismisses."""
    run_process(
        [
            str(caret_consumer),
            "canvas-menu-visibility",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="restore native Canvas text-field focus after a covered visibility change",
        timeout=10,
        check=True,
    )


@dataclass(frozen=True)
class MenuGeometry:
    """Owned popup dimensions and expected insets, independent of profile lookup."""

    width: int
    height: int
    padding: int
    row_height: int
    header_height: int
    margin: int


@pytest.mark.parametrize(
    "geometry",
    [
        pytest.param(MenuGeometry(128, 128, 1, 20, 20, 4), id="128x128"),
        pytest.param(MenuGeometry(128, 160, 1, 22, 20, 4), id="128x160"),
        pytest.param(MenuGeometry(176, 220, 4, 26, 24, 6), id="176x220"),
        pytest.param(MenuGeometry(240, 320, 6, 36, 28, 8), id="240x320"),
        pytest.param(MenuGeometry(320, 240, 4, 30, 26, 6), id="320x240"),
    ],
)
@pytest.mark.parametrize("titled", [False, True])
def test_native_menu_keeps_list_insets_when_created_and_resized(
    caret_consumer: Path,
    geometry: MenuGeometry,
    *,
    titled: bool,
) -> None:
    """Popup bounds include both row insets and stay bounded by the resized surface."""
    run_process(
        [
            str(caret_consumer),
            "menu-title-insets" if titled else "menu-insets",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            str(geometry.width),
            str(geometry.height),
            str(geometry.padding),
            str(geometry.row_height),
            str(geometry.header_height),
            str(geometry.margin),
        ],
        name=f"native menu insets at {geometry.width}x{geometry.height}, title={titled}",
        timeout=10,
        check=True,
    )

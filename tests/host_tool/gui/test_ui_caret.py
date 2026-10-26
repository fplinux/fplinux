# SPDX-License-Identifier: GPL-2.0-only
"""Native LVGL caret pixels and text-field lifecycle with a controlled clock."""

from __future__ import annotations

import hashlib
import json
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
    latin = (
        " /0123456789AHVxFirstSecondThirdFourthFifthSixthLast"
        "Read onlyWSpacerAbcToijBackHelpClearCancelOpen"
    )
    codes = tuple(sorted({*map(ord, latin), 0x0410, 0x0422, 0x0430, 0x0440, 0x044F}))
    face.write_bytes(synthetic_face(codes))
    specs = [
        fplinux_ui_fonts.FontSpec(
            name,
            face.name,
            size,
            digits=False,
            ranges=((face.name, codes),),
        )
        for name, size in (("caret-14", 14), ("menu-10", 10), ("menu-title-20", 20))
    ]
    (directory / "fplinux-font-sources.json").write_text(
        json.dumps(
            {
                "converter_version": "1.5.3",
                "sources": {
                    face.name: {
                        "url": "https://example.invalid/caret.ttf",
                        "algorithm": "sha256",
                        "checksum": hashlib.sha256(face.read_bytes()).hexdigest(),
                    }
                },
            }
        )
    )
    fplinux_ui_fonts.build(
        specs, directory, directory, "/opt/quality/node-tools/node_modules/.bin/lv_font_conv"
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
            *(str(directory / spec.c_name) for spec in specs),
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


def test_native_viewport_releases_scrollbar_space_when_text_fits(
    caret_consumer: Path,
) -> None:
    """Wrapped text returns to full width after overflow; OFF reserves no lane."""
    run_process(
        [
            str(caret_consumer),
            "conditional-scrollbar",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="resize real wrapped text through fit, overflow and fit again",
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
    ("width", "height"),
    [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)],
)
def test_native_list_form_menu_and_read_only_text_share_scrollbar_appearance(
    caret_consumer: Path, width: int, height: int
) -> None:
    """Widgets use a two-pixel thumb with one-pixel insets and hide it when content fits."""
    run_process(
        [
            str(caret_consumer),
            "scrollbars",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            str(width),
            str(height),
        ],
        name=f"native list, form, menu and read-only text scrollbars at {width}x{height}",
        timeout=10,
        check=True,
    )


@dataclass(frozen=True)
class ContentGeometry:
    """Screen dimensions and independent expected outer and inner insets."""

    width: int
    height: int
    margin: int
    field_padding: int
    alert_margin: int
    alert_padding: int


@pytest.mark.parametrize(
    "geometry",
    [
        pytest.param(ContentGeometry(128, 128, 4, 2, 2, 3), id="128x128"),
        pytest.param(ContentGeometry(128, 160, 4, 2, 2, 3), id="128x160"),
        pytest.param(ContentGeometry(176, 220, 6, 3, 6, 6), id="176x220"),
        pytest.param(ContentGeometry(240, 320, 8, 4, 8, 8), id="240x320"),
        pytest.param(ContentGeometry(320, 240, 6, 3, 6, 6), id="320x240"),
    ],
)
def test_native_scrolling_content_keeps_margins_and_space_for_its_thumb(
    caret_consumer: Path, geometry: ContentGeometry
) -> None:
    """Form text and images, read-only fields and bordered alerts keep clear insets."""
    run_process(
        [
            str(caret_consumer),
            "content-insets",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            str(geometry.width),
            str(geometry.height),
            str(geometry.margin),
            str(geometry.field_padding),
            str(geometry.alert_margin),
            str(geometry.alert_padding),
        ],
        name=f"native scrolling content insets at {geometry.width}x{geometry.height}",
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


def test_native_alert_style_matches_custom_alert_semantics(caret_consumer: Path) -> None:
    """Native frames and titles distinguish ordinary, warning and error messages."""
    run_process(
        [
            str(caret_consumer),
            "alert-style",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="render native alert kinds with independent theme colours",
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
    margin: int
    row_height: int
    header_height: int
    plain_width: int
    titled_width: int
    origin_x: tuple[tuple[int, int], ...]


@pytest.mark.parametrize(
    "geometry",
    [
        pytest.param(
            MenuGeometry(128, 128, 4, 20, 20, 44, 68, ((0, 0), (42, 30), (84, 60))),
            id="128x128",
        ),
        pytest.param(
            MenuGeometry(128, 160, 4, 22, 20, 44, 68, ((0, 0), (42, 30), (84, 60))),
            id="128x160",
        ),
        pytest.param(
            MenuGeometry(176, 220, 6, 26, 24, 48, 72, ((0, 0), (64, 52), (128, 104))),
            id="176x220",
        ),
        pytest.param(
            MenuGeometry(240, 320, 8, 36, 28, 52, 76, ((0, 0), (94, 82), (188, 164))),
            id="240x320",
        ),
        pytest.param(
            MenuGeometry(320, 240, 6, 30, 26, 48, 72, ((0, 0), (136, 124), (272, 248))),
            id="320x240",
        ),
    ],
)
@pytest.mark.parametrize("titled", [False, True])
@pytest.mark.parametrize("origin", [0, 1, 2], ids=["left", "centre", "right"])
def test_native_menu_sizes_to_labels_and_whole_rows_above_its_origin(
    caret_consumer: Path,
    geometry: MenuGeometry,
    origin: int,
    *,
    titled: bool,
) -> None:
    """Literal font advances establish compact widths, title sizing and row boundaries."""
    run_process(
        [
            str(caret_consumer),
            "menu-title-insets" if titled else "menu-insets",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            str(geometry.width),
            str(geometry.height),
            str(geometry.margin),
            str(geometry.row_height),
            str(geometry.header_height),
            str(geometry.titled_width if titled else geometry.plain_width),
            str(origin),
            str(geometry.origin_x[origin][int(titled)]),
        ],
        name=(
            f"native compact menu at {geometry.width}x{geometry.height}, "
            f"title={titled}, origin={origin}"
        ),
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("scenario", "width"),
    [("single", 22), ("long", 240), ("icons", 84), ("missing-icon", 52)],
)
def test_native_menu_measures_full_text_and_only_decoded_icon_columns(
    caret_consumer: Path, scenario: str, width: int
) -> None:
    """A short action stays narrow; translations clamp; missing images add no column."""
    run_process(
        [
            str(caret_consumer),
            f"menu-width-{scenario}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            str(width),
        ],
        name=f"native menu width with {scenario}",
        timeout=10,
        check=True,
    )


def test_native_menu_resize_restores_natural_width_and_reveals_last_row(
    caret_consumer: Path,
) -> None:
    """Screen changes retain the origin, unclamped width and a fully visible selection."""
    run_process(
        [
            str(caret_consumer),
            "menu-resize",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="resize a compact native menu with its last command selected",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("scenario", ["fit", "overflow"])
def test_popup_choice_uses_natural_height_up_to_available_surface(
    caret_consumer: Path, scenario: str
) -> None:
    run_process(
        [
            str(caret_consumer),
            f"choice-height-{scenario}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"native popup choice height near capacity: {scenario}",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("presentation", ["stored", "virtual"])
def test_list_icon_roles_preserve_colour_glyph_recolour_and_selected_tile_ring(
    caret_consumer: Path, presentation: str
) -> None:
    run_process(
        [
            str(caret_consumer),
            f"list-icons-{presentation}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"native {presentation} list icon role pixels and row changes",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("kind", ["list", "form", "text-box", "alert", "canvas"])
def test_screen_key_filter_precedes_commands_and_native_element_routing(
    caret_consumer: Path, kind: str
) -> None:
    run_process(
        [
            str(caret_consumer),
            f"key-filter-{kind}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"native {kind} screen key filter routing",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("transition", ["cover", "replace", "replace-unclaimed"])
def test_screen_filter_retains_key_ownership_across_screen_changes(
    caret_consumer: Path, transition: str
) -> None:
    run_process(
        [
            str(caret_consumer),
            f"filter-{transition}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"native screen filter key ownership after {transition}",
        timeout=10,
        check=True,
    )


def test_list_focus_listener_distinguishes_navigation_from_marked_values(
    caret_consumer: Path,
) -> None:
    run_process(
        [
            str(caret_consumer),
            "list-focus-listener",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="native List focus listener preserves marked-value and programmatic semantics",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize(
    ("lcd", "surface_height", "bounds"),
    [
        pytest.param((128, 128), 96, (2, 0, 123, 96), id="128x128"),
        pytest.param((128, 160), 128, (2, 16, 123, 96), id="128x160"),
        pytest.param((176, 220), 178, (5, 7, 166, 164), id="176x220"),
        pytest.param((240, 320), 268, (24, 44, 192, 180), id="240x320"),
        pytest.param((320, 240), 196, (20, 33, 280, 130), id="320x240"),
    ],
)
@pytest.mark.parametrize("context", ["text-box", "form", "form-scrolled", "raw-top", "raw-low"])
def test_native_symbols_center_in_surface_independent_of_field(
    caret_consumer: Path,
    lcd: tuple[int, int],
    surface_height: int,
    bounds: tuple[int, int, int, int],
    context: str,
) -> None:
    """Short star opens one literal rectangle despite a field's label, position or scroll."""
    run_process(
        [
            str(caret_consumer),
            f"symbols-{context}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
            *(str(value) for value in (*lcd, surface_height, *bounds)),
        ],
        name=f"native {context} Symbols in {lcd[0]}x{surface_height} surface",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("shape", ["square", "landscape"])
def test_native_form_image_keeps_shape_through_appearance_and_command_changes(
    caret_consumer: Path, shape: str
) -> None:
    """Real image pixels retain their square or landscape bounds in links and buttons."""
    run_process(
        [
            str(caret_consumer),
            f"form-image-{shape}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"native {shape} Form image through appearance and command changes",
        timeout=10,
        check=True,
    )


def test_native_framed_form_image_fits_the_actual_scrolling_viewport(
    caret_consumer: Path,
) -> None:
    """A wide link image leaves the real B1 thumb lane clear without losing its ratio."""
    run_process(
        [
            str(caret_consumer),
            "form-image-overflow",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name="native framed image inside an overflowing Form viewport",
        timeout=10,
        check=True,
    )


@pytest.mark.parametrize("scenario", ["default", "clear", "explicit", "missing"])
def test_native_options_resolves_semantic_glyphs_and_renders_the_selected_resource(
    caret_consumer: Path, scenario: str
) -> None:
    """Runtime lookup is controlled; real menu decoding shows each distinct resource's pixels."""
    run_process(
        [
            str(caret_consumer),
            f"command-glyph-{scenario}",
            str(ROOT / "tests/host_tool/input/fixtures/input-languages"),
        ],
        name=f"native Options glyph resolution and pixels: {scenario}",
        timeout=10,
        check=True,
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Capture the MRE and phoneME shells through their actual UI runtime."""

from __future__ import annotations

import json
import os
import shutil
from pathlib import Path

from tests import ROOT
from tests.process import run_process

BAR_HEIGHTS = {(128, 128): 32, (128, 160): 34, (176, 220): 43, (240, 320): 52, (320, 240): 41}
ROW_HEIGHTS = {(128, 128): 24, (128, 160): 23, (176, 220): 27, (240, 320): 33, (320, 240): 32}


def capture_menu_frames(  # noqa: PLR0913 -- peer, geometry and scenario inputs remain explicit.
    native: Path,
    directory: Path,
    screen: tuple[int, int],
    locale: str,
    theme: str,
    *,
    application: str,
    document: Path,
    scenario: str | None = None,
) -> None:
    width, height = screen
    result = run_process(
        [
            str(native / "menu-screens"),
            str(native / application),
            str(ROOT / "tests/host_tool/files/native/keyboard.xkb"),
            str(directory / "session.sock"),
            str(directory),
            str(width),
            str(height),
            locale,
            f"org.fplinux.{application}",
            str(document),
            theme,
            *([scenario] if scenario else []),
        ],
        name=f"actual {application} menus at {width}x{height} ({locale}/{theme})",
        env=os.environ
        | {
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_UI_LOCALE_DIRECTORY": str(native / "locale"),
            "FPLINUX_NATIVE_GLYPHS": str(native / "icons/usr/share/fplinux/glyphs"),
            "FPLINUX_MRE_TEST_ENGINE": str(native / "engine-peer"),
            "FPLINUX_MRE_ENGINE_RECORD": str(directory / "engine.log"),
            "FPLINUX_APP_FRAME_RECORD": str(directory / "frames.log"),
            "FPLINUX_APP_SAVE_FAULT": str(directory / "save-fault"),
            "XDG_STATE_HOME": str(directory / "state"),
        },
        timeout=30,
    )
    (directory / "surface.json").write_text(
        json.dumps({"width": width, "height": height - BAR_HEIGHTS[screen]}) + "\n",
        encoding="utf-8",
    )
    (directory / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            directory,
            Path(log_root)
            / f"{application}-{scenario or 'menus'}-{width}x{height}-{locale}-{theme}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock", "state", "engine.log", "*.jar", "*.vxp"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "application settings scenario checked\n"
        if scenario
        else "application menus captured from real Wayland frames\n"
    )
    if scenario:
        return
    names = (
        "home",
        "home-running",
        "settings-global",
        "settings-global-choice-scale",
        "settings-program",
        "settings-program-choice-scale",
        "recent",
        "scope",
    )
    frames = [(directory / f"{name}.rgb565").read_bytes() for name in names]
    assert all(len(frame) == width * (height - BAR_HEIGHTS[screen]) * 2 for frame in frames)
    assert frames[0] != frames[1]
    assert frames[2] != frames[3]
    assert frames[4] != frames[5]
    scale = "Масштаб" if locale == "ru" else "Scale"
    assert f"|{scale}|" in (directory / "settings-global.labels").read_text(encoding="utf-8")
    home_rows = (
        ("Открыть VXP" if application == "mre" else "Открыть", "Недавние", "Настройки")
        if locale == "ru"
        else ("Open VXP" if application == "mre" else "Open", "Recent", "Settings")
    )
    settings_rows: tuple[str, ...] = (
        ("Масштаб", "Поворот", "Указатель" if application == "mre" else "Размер")
        if locale == "ru"
        else ("Scale", "Rotation", "Pointer" if application == "mre" else "Size")
    )
    jit_row_scrolls = application == "phoneme" and screen == (128, 128)
    if application == "phoneme" and not jit_row_scrolls:
        settings_rows += ("JIT",)
    layouts: tuple[tuple[str, tuple[str, ...]], ...] = (
        ("home", home_rows),
        ("home-running", ("Продолжить" if locale == "ru" else "Continue", *home_rows)),
        ("settings-global", settings_rows),
        ("settings-program", settings_rows),
    )
    if jit_row_scrolls:
        layouts += (("settings-global-jit", ("JIT",)), ("settings-program-jit", ("JIT",)))
    for name, captions in layouts:
        entries = json.loads((directory / f"{name}.layout.json").read_text(encoding="utf-8"))
        for caption in captions:
            found = [entry for entry in entries if entry["text"] == caption]
            # The smallest running menu scrolls to the last row only when focused.
            if screen == (128, 128) and name == "home-running" and caption == home_rows[-1]:
                continue
            assert len(found) == 1, (name, caption, entries)
            row = found[0]
            assert row["row_height"] == ROW_HEIGHTS[screen], (name, row)
            assert row["images"] == 1, (name, row)
            assert row["natural_width"] <= row["width"], (name, row)
            assert 0 <= row["top"] <= row["bottom"] < height - BAR_HEIGHTS[screen], (name, row)
            for value in entries:
                if value["text"] not in captions and value["row_top"] == row["row_top"]:
                    assert value["natural_width"] <= value["width"], (name, value)


def preference_values(path: Path) -> dict[str, str]:
    return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8").splitlines())


def check_settings_roundtrip(
    native: Path, directory: Path, application: str, document: Path
) -> None:
    root = directory / f"state/fplinux/org.fplinux.{application}"
    third = "pointer-mode" if application == "mre" else "canvas-size"
    global_third = "1" if application == "mre" else "3"
    for scenario in ("roundtrip-write", "roundtrip-reset", "roundtrip-inherit"):
        capture_menu_frames(
            native,
            directory,
            (240, 320),
            "en",
            "light",
            application=application,
            document=document,
            scenario=scenario,
        )
        global_values = preference_values(root / "preferences")
        assert json.loads(global_values["view-settings"]) == {"scale": "integer", "rotation": "90"}
        assert global_values[third] == global_third
        programs = list((root / "programs").glob("*/preferences"))
        assert len(programs) == 1
        program_values = preference_values(programs[0])
        if scenario == "roundtrip-write":
            assert json.loads(program_values["view-settings"]) == {
                "scale": "one-to-one",
                "rotation": "270",
            }
            assert program_values[third] == "0"
        else:
            assert "view-settings" not in program_values
            assert third not in program_values


def check_settings_save_retry(
    native: Path, directory: Path, application: str, document: Path, operation: str
) -> None:
    root = directory / f"state/fplinux/org.fplinux.{application}"
    root.mkdir(parents=True, mode=0o700)
    (root / "preferences").write_text(
        'view-settings={"scale":"one-to-one","rotation":"270"}\n', encoding="utf-8"
    )
    capture_menu_frames(
        native,
        directory,
        (240, 320),
        "en",
        "light",
        application=application,
        document=document,
        scenario=f"retry-{operation}",
    )
    failed = preference_values(directory / "failed.preferences")
    assert json.loads(failed["view-settings"]) == {
        "scale": "one-to-one" if operation == "file" else "integer",
        "rotation": "270",
    }
    saved = preference_values(root / "preferences")
    assert json.loads(saved["view-settings"]) == {"scale": "integer", "rotation": "270"}


def check_settings_cross_scope(
    native: Path, directory: Path, application: str, document: Path, failed_scope: str
) -> None:
    root = directory / f"state/fplinux/org.fplinux.{application}"
    root.mkdir(parents=True, mode=0o700)
    (root / "preferences").write_text(
        'view-settings={"scale":"fit","rotation":"0"}\n', encoding="utf-8"
    )
    capture_menu_frames(
        native,
        directory,
        (240, 320),
        "en",
        "light",
        application=application,
        document=document,
        scenario=f"cross-{failed_scope}",
    )
    global_values = preference_values(root / "preferences")
    programs = list((root / "programs").glob("*/preferences"))
    if failed_scope == "program":
        assert json.loads(global_values["view-settings"]) == {"scale": "fit", "rotation": "90"}
        assert programs == []
    else:
        assert json.loads(global_values["view-settings"]) == {"scale": "fit", "rotation": "0"}
        assert len(programs) == 1
        program_values = preference_values(programs[0])
        assert json.loads(program_values["view-settings"]) == {
            "scale": "integer",
            "rotation": "90",
        }

# SPDX-License-Identifier: GPL-2.0-only
"""Real API Demos input, preferences and pixels through a native Wayland peer.

The peer controls session replies and reports unavailable media capabilities.
It does not emulate a motor, mixer or physical sound output.
"""

from __future__ import annotations

import os
import shutil
import struct
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "site/scripts/screens"
CAPTURES = (
    "apidemo-no-results",
    "apidemo-catalogue",
    "apidemo-virtual-selection",
    "apidemo-choice",
    "apidemo-preferences",
    "apidemo-metrics",
    "apidemo-fullscreen",
    "apidemo-normal-view",
    "apidemo-frames",
    "apidemo-audio-unavailable",
    "apidemo-vibration-error",
)


@pytest.fixture(scope="module")
def apidemo_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[Path]:
    """Build the real app and peer against the shared native UI inputs."""
    with c_build_directory(tmp_path_factory, "apidemo-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-apidemo.sh"), str(directory), str(ui_host_inputs)],
            name="build actual API Demos and native capture peer",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_apidemo_help_precedes_preferences_and_ui(
    apidemo_native: Path, tmp_path: Path, arguments: list[str]
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(apidemo_native / "apidemo"), *arguments],
        name="API Demos help before resources",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-apidemo-display",
        },
        timeout=10,
    )
    assert result.returncode == 0
    assert "Usage: fplinux-example-apidemo" in result.stdout
    assert result.stderr == ""
    assert not state.exists()


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_apidemo_invalid_arguments_precede_preferences_and_ui(
    apidemo_native: Path, tmp_path: Path, argument: str
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(apidemo_native / "apidemo"), argument],
        name="API Demos invalid arguments before resources",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-apidemo-display",
        },
        timeout=10,
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-example-apidemo:" in result.stderr
    assert not state.exists()


def test_actual_apidemo_preferences_distinguish_memory_file_and_confirmed_save(
    apidemo_native: Path, tmp_path: Path
) -> None:
    result = run_process(
        [
            str(apidemo_native / "apidemo-screens"),
            "240",
            "320",
            str(apidemo_native / "apidemo"),
            str(SCREENS / "apidemo-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "en",
            "storage-failure",
        ],
        name="actual API Demos preferences under controlled write and sync failures",
        env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "apidemo-storage: memory differs from file after ENOSPC; autosave has no modal spam; "
        "published sync failure remains unconfirmed; retry reopens 2\n"
    )
    preferences = tmp_path / "state/fplinux/org.fplinux.example.apidemo/preferences"
    assert "locale.count=2\n" in preferences.read_text(encoding="utf-8")


@pytest.mark.parametrize(
    ("screen", "surface", "locale"),
    [
        ((128, 128), (128, 96), "en"),
        ((128, 160), (128, 128), "en"),
        ((176, 220), (176, 178), "en"),
        ((240, 320), (240, 268), "en"),
        ((320, 240), (320, 196), "en"),
        ((240, 320), (240, 268), "ru"),
    ],
)
def test_actual_apidemo_search_choices_preferences_and_rendering(
    apidemo_native: Path,
    tmp_path_factory: pytest.TempPathFactory,
    screen: tuple[int, int],
    surface: tuple[int, int],
    locale: str,
) -> None:
    output = tmp_path_factory.mktemp(f"apidemo-{screen[0]}-{screen[1]}-{locale}")
    try:
        result = run_process(
            [
                str(apidemo_native / "apidemo-screens"),
                *map(str, screen),
                str(apidemo_native / "apidemo"),
                str(SCREENS / "apidemo-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
                locale,
                "all",
            ],
            name=f"exercise actual API Demos at {screen[0]}x{screen[1]} ({locale})",
            env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
            timeout=60,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        captures: tuple[str, ...]
        if locale == "ru":
            assert result.stdout.endswith(
                "apidemo: Cyrillic uppercase search found translated list title\n"
            )
            captures = ("apidemo-russian-search",)
        else:
            for behavior in (
                "Options Clear empties search with Open/Back",
                "virtual row 2 selected",
                "real form choice changed to Large",
                "preferences reopened at 1; saved 2; hidden/show/close preserved state",
                "unavailable audio; asynchronous vibration error; close cleanup",
            ):
                assert behavior in result.stdout
            preferences = output / "state/fplinux/org.fplinux.example.apidemo/preferences"
            saved = dict(
                line.split("=", 1) for line in preferences.read_text(encoding="utf-8").splitlines()
            )
            assert saved["locale.name"] == ""
            assert saved["locale.count"] == "2"
            assert preferences.stat().st_mode & 0o777 == 0o600
            assert preferences.parent.stat().st_mode & 0o777 == 0o700
            captures = CAPTURES
        for name in captures:
            width, height = screen if name == "apidemo-fullscreen" else surface
            frame = output / f"{name}.rgb565"
            pixels = frame.read_bytes()
            assert len(pixels) == width * height * 2
            assert len(set(struct.iter_unpack("<H", pixels))) > 8
            assert f"{name}: {width}x{height}\n" in result.stdout
            run_process(
                [
                    "python3",
                    str(SCREENS / "rgb565-png.py"),
                    str(width),
                    str(height),
                    str(frame),
                    str(output / f"{name}.png"),
                ],
                name=f"encode actual API Demos capture {name}",
                timeout=10,
                check=True,
            )
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / f"apidemo-screens-{screen[0]}x{screen[1]}-{locale}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)


@pytest.mark.parametrize(
    ("scenario", "expected"),
    [
        ("dates", "Dates small-buffer row reports false, ERANGE"),
        # U+0421 is Cyrillic capital ES, distinct from the Latin C lookalike.
        ("keyboard", "Keyboard types СП; Backspace leaves \u0421; phone digit gives \u04212"),
        ("textarea", "Text area types СП; Backspace leaves \u0421; phone digit gives \u04212"),
        (
            "buttonmatrix",
            "Button matrix ignores \u0431, preserves selected 2; ASCII 1/*/# type their keys",
        ),
        ("whole-escape", "external Escape leaves Whole surface"),
        ("whole-phone-back", "phone Back leaves Whole surface"),
        (
            "canvas-clear-hold",
            "held Clear empties Canvas field; release stays; next Back leaves",
        ),
    ],
    ids=[
        "dates",
        "keyboard",
        "textarea",
        "buttonmatrix",
        "whole-escape",
        "whole-phone-back",
        "canvas-clear-hold",
    ],
)
def test_actual_apidemo_date_errors_widgets_and_canvas_keys(
    apidemo_native: Path,
    tmp_path_factory: pytest.TempPathFactory,
    scenario: str,
    expected: str,
) -> None:
    """Exercise errors, text editing and physical key routing in the actual app."""
    output = tmp_path_factory.mktemp(f"apidemo-{scenario}")
    try:
        result = run_process(
            [
                str(apidemo_native / "apidemo-screens"),
                "240",
                "320",
                str(apidemo_native / "apidemo"),
                str(SCREENS / "apidemo-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
                "en",
                scenario,
            ],
            name=f"exercise real API Demos {scenario}",
            env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
            timeout=15,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.endswith(f"apidemo: {expected}\n")
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / f"apidemo-{scenario}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)


def test_actual_apidemo_numeric_canvas_field_has_no_newline_command(
    apidemo_native: Path, tmp_path_factory: pytest.TempPathFactory
) -> None:
    output = tmp_path_factory.mktemp("apidemo-numeric")
    try:
        result = run_process(
            [
                str(apidemo_native / "apidemo-screens"),
                "240",
                "320",
                str(apidemo_native / "apidemo"),
                str(SCREENS / "apidemo-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
                "en",
                "numeric",
            ],
            name="exercise numeric API Demos Canvas field commands",
            env={"FPLINUX_APIDEMO_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language")},
            timeout=15,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.endswith("apidemo: numeric Canvas field offers no New line command\n")
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / "apidemo-numeric",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)

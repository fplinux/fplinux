# SPDX-License-Identifier: GPL-2.0-only
"""Actual Calculator input, persistence and captures through a native Wayland peer."""

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
CAPTURE_STATES = (
    "calculator-pending",
    "calculator-result",
    "calculator-number",
    "calculator-error",
    "calculator-menu",
)


@pytest.fixture(scope="module")
def calculator_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[Path]:
    """Build the real app and peer against the shared native UI inputs."""
    with c_build_directory(tmp_path_factory, "calculator-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-calculator.sh"), str(directory), str(ui_host_inputs)],
            name="build the real Calculator and native capture peer",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_calculator_help_precedes_arguments_preferences_and_ui(
    calculator_native: Path, tmp_path: Path, arguments: list[str]
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(calculator_native / "calculator"), *arguments],
        name="Calculator help before resource access",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-calculator-display",
        },
        timeout=10,
    )
    assert result.returncode == 0
    assert "Usage: fplinux-calculator" in result.stdout
    assert result.stderr == ""
    assert not state.exists()


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_calculator_rejects_arguments_before_preferences_and_ui(
    calculator_native: Path, tmp_path: Path, argument: str
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(calculator_native / "calculator"), argument],
        name="Calculator invalid arguments before resource access",
        env={
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-calculator-display",
        },
        timeout=10,
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-calculator:" in result.stderr
    assert not state.exists()


def test_actual_calculator_storage_failure_retains_draft_and_honors_close_choices(
    calculator_native: Path, tmp_path: Path
) -> None:
    result = run_process(
        [
            str(calculator_native / "calculator-screens"),
            "240",
            "320",
            str(calculator_native / "calculator"),
            str(SCREENS / "calculator-keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "storage-failure",
        ],
        name="actual Calculator draft and close decisions after a failed write",
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "calculator-storage: Home retained draft; Stay continued input; Retry saved; "
        "Discard reopened old state\n"
    )


@pytest.mark.parametrize(
    ("screen", "surface"),
    [((128, 160), (128, 128)), ((240, 320), (240, 268)), ((320, 240), (320, 196))],
)
def test_actual_calculator_input_holds_persistence_and_rendering_at_each_profile(
    calculator_native: Path,
    tmp_path_factory: pytest.TempPathFactory,
    screen: tuple[int, int],
    surface: tuple[int, int],
) -> None:
    """The host app draws whole labels, routes real keys and resumes a saved operation."""
    output = tmp_path_factory.mktemp(f"calculator-{screen[0]}")
    try:
        result = run_process(
            [
                str(calculator_native / "calculator-screens"),
                *map(str, screen),
                str(calculator_native / "calculator"),
                str(SCREENS / "calculator-keyboard.xkb"),
                str(output / "session.sock"),
                str(output),
            ],
            name=f"capture actual Calculator at {screen[0]}x{screen[1]}",
            timeout=30,
        )
        (output / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        assert result.returncode == 0, result.stdout + result.stderr
        width, height = surface
        assert (output / "frame-size").read_text(encoding="utf-8") == f"{width} {height}\n"
        for text in ("-12345678901.2", "Error", "54"):
            assert f"calculator-number {width}x{height} {text}: " in result.stdout
        for scenario in ("leave", "hidden", "menu", "background", "another-key"):
            assert f"calculator-clear-cancel {scenario}: preserved\n" in result.stdout
        assert result.stdout.endswith(
            "calculator: phone and keyboard; menu; Clear holds; hidden/background/close "
            "persistence; reopen=54; Back preserves result\n"
        )
        states = []
        for state in CAPTURE_STATES:
            frame = output / f"{state}.rgb565"
            pixels = frame.read_bytes()
            assert len(pixels) == width * height * 2
            assert len(set(struct.iter_unpack("<H", pixels))) > 8
            assert f"{state}: {width}x{height}\n" in result.stdout
            states.append(pixels)
            run_process(
                [
                    "python3",
                    str(SCREENS / "rgb565-png.py"),
                    str(width),
                    str(height),
                    str(frame),
                    str(output / f"{state}.png"),
                ],
                name=f"encode the actual Calculator {state} capture",
                timeout=10,
                check=True,
            )
        assert len(set(states)) == 5
        preferences = output / "state/fplinux/org.fplinux.calculator/preferences"
        assert preferences.stat().st_mode & 0o777 == 0o600
        assert preferences.parent.stat().st_mode & 0o777 == 0o700
    finally:
        log_root = os.environ.get("FPLINUX_LOG_ROOT")
        if log_root:
            shutil.copytree(
                output,
                Path(log_root) / f"calculator-screens-{screen[0]}x{screen[1]}",
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("session.sock"),
            )
        shutil.rmtree(output)

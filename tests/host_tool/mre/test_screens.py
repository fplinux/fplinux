# SPDX-License-Identifier: GPL-2.0-only
"""Actual MRE UI/lifecycle with a controlled execution peer, without ARM execution."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HERE = ROOT / "tests/host_tool/mre"


@pytest.fixture(scope="module")
def mre_native(tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "mre-native") as directory:
        run_process(
            [
                "sh",
                str(HERE / "fixtures/build.sh"),
                str(ROOT),
                str(directory),
                str(ui_host_inputs),
            ],
            name="build actual MRE UI and execution/session peers",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize("arguments", [["--help"], ["--unknown", "--help"]])
def test_mre_help_precedes_state_ui_and_child_access(
    mre_native: Path, tmp_path: Path, arguments: list[str]
) -> None:
    state = tmp_path / "missing-state"
    result = run_process(
        [str(mre_native / "mre"), *arguments],
        name="MRE help without runtime resources",
        timeout=10,
        env={"XDG_STATE_HOME": str(state), "WAYLAND_DISPLAY": "missing-mre-display"},
    )
    assert result.returncode == 0
    assert "Usage: fplinux-mre" in result.stdout
    assert result.stderr == ""
    assert not state.exists()


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_mre_first_frame_home_replace_cancel_and_close(
    mre_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str
) -> None:
    (tmp_path / "first.vxp").write_bytes(b"A execution-peer fixture")
    (tmp_path / "second.vxp").write_bytes(b"B execution-peer fixture")
    (tmp_path / "invalid.vxp").write_bytes(b"- rejected by execution peer")
    record = tmp_path / "engine.log"
    result = run_process(
        [
            str(mre_native / "mre-screens"),
            str(mre_native / "mre"),
            str(ROOT / "tests/host_tool/files/native/keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            *map(str, screen),
            locale,
        ],
        name=f"actual MRE lifecycle at {screen[0]}x{screen[1]} ({locale})",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_UI_LOCALE_DIRECTORY": str(mre_native / "locale"),
            "FPLINUX_MRE_TEST_ENGINE": str(mre_native / "engine-peer"),
            "FPLINUX_MRE_ENGINE_RECORD": str(record),
            "XDG_STATE_HOME": str(tmp_path / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "mre-ui: first frame; retained Home/Resume; invalid preflight; "
        "cancel preserves guest; consent replaces; close cancels\n"
    )
    records = [line.split() for line in record.read_text().splitlines()]
    started = [(kind, program, pid) for kind, program, pid in records if kind == "S"]
    assert [program for _, program, _ in started] == ["A", "B"]
    first_pid = started[0][2]
    assert all(pid == first_pid for kind, program, pid in records if program == "A")
    assert sum(kind == "K" and program == "A" for kind, program, _ in records) == 2


@pytest.mark.parametrize("screen", [(128, 128), (240, 320)])
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize(
    "first_ui",
    [pytest.param(("editor", "I"), id="editor"), pytest.param(("selector", "S"), id="selector")],
)
def test_actual_mre_first_modal_accepts_after_commit_and_releases_open_request(
    mre_native: Path,
    tmp_path: Path,
    screen: tuple[int, int],
    locale: str,
    first_ui: tuple[str, str],
) -> None:
    run_first_modal(mre_native, tmp_path, screen, locale, first_ui=first_ui)


def test_actual_mre_malformed_first_editor_reports_error_without_accepting(
    mre_native: Path, tmp_path: Path
) -> None:
    run_first_modal(mre_native, tmp_path, (240, 320), "en", first_ui=("malformed", "X"))


@pytest.mark.parametrize(
    "first_ui",
    [
        ("editor-close", "I"),
        ("editor-close-options", "I"),
        ("editor-end", "E"),
        ("editor-end-options", "E"),
    ],
)
def test_actual_mre_editor_detaches_before_guest_or_application_cleanup(
    mre_native: Path, tmp_path: Path, first_ui: tuple[str, str]
) -> None:
    """Real frontend cleanup leaves no editor callback using the released guest mapping."""
    run_first_modal(mre_native, tmp_path, (240, 320), "en", first_ui=first_ui)


@pytest.mark.parametrize("locale", ["en", "ru"])
def test_actual_mre_idle_timer_sleeps_resumes_validation_and_reaps_hidden_guest(
    mre_native: Path, tmp_path: Path, locale: str
) -> None:
    """Real frontend scheduling and picture retention; the guest is a controlled peer."""
    run_first_modal(mre_native, tmp_path, (240, 320), locale, first_ui=("idle-background", "P"))


def run_first_modal(
    mre_native: Path,
    directory: Path,
    screen: tuple[int, int],
    locale: str,
    *,
    first_ui: tuple[str, str],
) -> None:
    scenario, program = first_ui
    (directory / "first.vxp").write_bytes(program.encode() + b" first-modal fixture")
    (directory / "second.vxp").write_bytes(b"B execution-peer fixture")
    (directory / "invalid.vxp").write_bytes(b"- rejected by execution peer")
    record = directory / "engine.log"
    result = run_process(
        [
            str(mre_native / "mre-screens"),
            str(mre_native / "mre"),
            str(ROOT / "tests/host_tool/files/native/keyboard.xkb"),
            str(directory / "session.sock"),
            str(directory),
            *map(str, screen),
            locale,
            scenario,
        ],
        name=f"actual MRE {scenario} first surface at {screen[0]}x{screen[1]} ({locale})",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_UI_LOCALE_DIRECTORY": str(mre_native / "locale"),
            "FPLINUX_MRE_TEST_ENGINE": str(mre_native / "engine-peer"),
            "FPLINUX_MRE_ENGINE_RECORD": str(record),
            "FPLINUX_MRE_ENGINE_RELEASE": str(directory / "release"),
            "FPLINUX_MRE_ENGINE_END": str(directory / "end"),
            "FPLINUX_MRE_TICK_RECORD": str(directory / "ticks.log"),
            "XDG_STATE_HOME": str(directory / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "mre-ui: first modal document lifecycle checked\n"
    records = [line.split()[:2] for line in record.read_text().splitlines()]
    assert [entry for entry in records if entry[0] == "S"] == [["S", program]]
    if scenario == "idle-background":
        assert ["H", "P"] in records
        assert ["N", "P"] in records
        ticks = (directory / "ticks.log").read_text().splitlines()
        assert ticks[0] == "P"
        assert ticks[-1] == "P"
        assert ticks.count("R") >= 2
        assert "T" in ticks
        return
    if scenario.startswith("editor-"):
        assert (["N", program] in records) == (program == "E")
        return
    if scenario != "malformed":
        assert ["C" if scenario == "selector" else "E", program] in records

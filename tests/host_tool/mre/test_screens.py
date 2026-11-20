# SPDX-License-Identifier: GPL-2.0-only
"""Actual MRE UI/lifecycle with a controlled execution peer, without ARM execution."""

from __future__ import annotations

import hashlib
import json
import os
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.host_tool.mre.menu_frames import (
    capture_menu_frames,
    check_rotation_only_inherits_global_settings,
    check_settings_cross_scope,
    check_settings_roundtrip,
    check_settings_save_retry,
)
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HERE = ROOT / "tests/host_tool/mre"


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
@pytest.mark.parametrize("locale", ["en", "ru"])
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_actual_mre_menu_frames(
    mre_native: Path, tmp_path: Path, screen: tuple[int, int], locale: str, theme: str
) -> None:
    """The execution peer supplies a guest; all menu widgets and pixels are real."""
    document = tmp_path / "first.vxp"
    document.write_bytes(b"A execution-peer fixture")
    capture_menu_frames(
        mre_native, tmp_path, screen, locale, theme, application="mre", document=document
    )


def test_actual_mre_settings_survive_reopen_and_reset_to_global_values(
    mre_native: Path, tmp_path: Path
) -> None:
    document = tmp_path / "first.vxp"
    document.write_bytes(b"A execution-peer fixture")
    check_settings_roundtrip(mre_native, tmp_path, "mre", document)


def test_actual_mre_rotation_only_inherits_changed_global_settings_on_reopen(
    mre_native: Path, tmp_path: Path
) -> None:
    document = tmp_path / "first.vxp"
    document.write_bytes(b"A execution-peer fixture")
    check_rotation_only_inherits_global_settings(mre_native, tmp_path, "mre", document)


@pytest.mark.parametrize("operation", ["file", "directory"])
def test_actual_mre_same_choice_retries_failed_save_and_clears_unsaved(
    mre_native: Path, tmp_path: Path, operation: str
) -> None:
    document = tmp_path / "first.vxp"
    document.write_bytes(b"A execution-peer fixture")
    check_settings_save_retry(mre_native, tmp_path, "mre", document, operation)


@pytest.mark.parametrize("failed_scope", ["program", "global"])
def test_actual_mre_success_in_other_scope_keeps_failed_scope_unsaved(
    mre_native: Path, tmp_path: Path, failed_scope: str
) -> None:
    document = tmp_path / "first.vxp"
    document.write_bytes(b"A execution-peer fixture")
    check_settings_cross_scope(mre_native, tmp_path, "mre", document, failed_scope)


@pytest.fixture(scope="module")
def mre_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
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
        (directory / "icons").symlink_to(ui_icon_resources, target_is_directory=True)
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
            "FPLINUX_APP_FRAME_RECORD": str(tmp_path / "frames.log"),
            "XDG_STATE_HOME": str(tmp_path / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "mre-ui: first frame; retained Home/Resume; invalid preflight; "
        "cancel preserves guest; consent replaces; close ready\n"
    )
    records = [line.split() for line in record.read_text().splitlines()]
    started = [(kind, program, pid) for kind, program, pid in records if kind == "S"]
    assert [program for _, program, _ in started] == ["A", "B"]
    preferences = (tmp_path / "state/fplinux/org.fplinux.mre/preferences").read_text()
    recent_line = next(
        line for line in preferences.splitlines() if line.startswith("recent-documents=")
    )
    recent = json.loads(recent_line.partition("=")[2].replace("\\\\", "\\"))
    assert [entry["path"] for entry in recent] == [
        str(tmp_path / "second.vxp"),
        str(tmp_path / "first.vxp"),
    ]
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


def test_actual_mre_slow_open_shows_one_busy_alert_until_the_first_frame(
    mre_native: Path, tmp_path: Path
) -> None:
    """Cancel, Back and Home during a slow check; the alert spans the check and the start."""
    run_first_modal(mre_native, tmp_path, (240, 320), "en", first_ui=("slow", "W"))


def test_actual_mre_replaced_editor_keeps_its_legends_until_the_new_frame(
    mre_native: Path, tmp_path: Path
) -> None:
    """Replacing a running editor waits on the editor's legends, not on its released commands."""
    run_first_modal(mre_native, tmp_path, (240, 320), "en", first_ui=("replace-editor", "I"))


def test_actual_mre_failure_after_the_first_frame_shows_the_error_after_the_alert(
    mre_native: Path, tmp_path: Path
) -> None:
    run_first_modal(mre_native, tmp_path, (240, 320), "en", first_ui=("failed-start", "F"))


def test_mre_rotated_view_maps_direction_to_guest_key(mre_native: Path, tmp_path: Path) -> None:
    (tmp_path / "first.vxp").write_bytes(b"A execution-peer fixture")
    prefs = tmp_path / "state/fplinux/org.fplinux.mre"
    prefs.mkdir(parents=True, mode=0o700)
    (prefs / "preferences").write_text('view-settings={"scale":"fit","rotation":"90"}\n')
    keys = tmp_path / "keys.log"
    result = run_process(
        [
            str(mre_native / "mre-screens"),
            str(mre_native / "mre"),
            str(ROOT / "tests/host_tool/files/native/keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "240",
            "320",
            "en",
            "rotation",
        ],
        name="MRE rotated view maps a physical Up press and release",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_UI_LOCALE_DIRECTORY": str(mre_native / "locale"),
            "FPLINUX_MRE_TEST_ENGINE": str(mre_native / "engine-peer"),
            "FPLINUX_MRE_ENGINE_RECORD": str(tmp_path / "engine.log"),
            "FPLINUX_MRE_KEY_RECORD": str(keys),
            "FPLINUX_APP_FRAME_RECORD": str(tmp_path / "frames.log"),
            "XDG_STATE_HOME": str(tmp_path / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert keys.read_text().splitlines() == ["KEY\t2\t-3", "KEY\t1\t-3"]


@pytest.mark.parametrize(("rotation", "point"), [(0, (160, 160)), (90, (120, 122))])
def test_mre_pointer_survives_home_and_releases_held_pen(
    mre_native: Path, tmp_path: Path, rotation: int, point: tuple[int, int]
) -> None:
    (tmp_path / "first.vxp").write_bytes(b"T execution-peer fixture")
    prefs = tmp_path / "state/fplinux/org.fplinux.mre"
    prefs.mkdir(parents=True, mode=0o700)
    (prefs / "preferences").write_text(
        f'view-settings={{"scale":"fit","rotation":"{rotation}"}}\npointer-mode=1\n'
    )
    keys = tmp_path / "keys.log"
    result = run_process(
        [
            str(mre_native / "mre-screens"),
            str(mre_native / "mre"),
            str(ROOT / "tests/host_tool/files/native/keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "128",
            "128",
            "en",
            "pointer-home",
        ],
        name=f"MRE pointer survives Home with rotation {rotation}",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_UI_LOCALE_DIRECTORY": str(mre_native / "locale"),
            "FPLINUX_MRE_TEST_ENGINE": str(mre_native / "engine-peer"),
            "FPLINUX_MRE_ENGINE_RECORD": str(tmp_path / "engine.log"),
            "FPLINUX_MRE_KEY_RECORD": str(keys),
            "FPLINUX_APP_FRAME_RECORD": str(tmp_path / "frames.log"),
            "XDG_STATE_HOME": str(tmp_path / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    x, y = point
    assert keys.read_text().splitlines() == [
        f"PEN\t1\t{x}\t{y}",
        f"PEN\t2\t{x}\t{y}",
        f"PEN\t1\t{x}\t{y}",
        f"PEN\t2\t{x}\t{y}",
    ]


def test_mre_recent_remove_only_available_for_existing_entry(
    mre_native: Path, tmp_path: Path
) -> None:
    program = tmp_path / "first.vxp"
    program.write_bytes(b"A execution-peer fixture")
    state_root = tmp_path / "state/fplinux/org.fplinux.mre"
    state_root.mkdir(parents=True, mode=0o700)
    identity = hashlib.sha256(os.fsencode(program)).hexdigest()
    guest_data = state_root / "apps" / identity / "save.dat"
    guest_data.parent.mkdir(parents=True, mode=0o700)
    guest_data.write_bytes(b"saved game")
    result = run_process(
        [
            str(mre_native / "mre-screens"),
            str(mre_native / "mre"),
            str(ROOT / "tests/host_tool/files/native/keyboard.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            "240",
            "320",
            "en",
            "recent-remove-last",
        ],
        name="MRE Recent hides Remove when empty and after removing its last entry",
        env={
            **os.environ,
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_UI_LOCALE_DIRECTORY": str(mre_native / "locale"),
            "FPLINUX_MRE_TEST_ENGINE": str(mre_native / "engine-peer"),
            "FPLINUX_MRE_ENGINE_RECORD": str(tmp_path / "engine.log"),
            "FPLINUX_APP_FRAME_RECORD": str(tmp_path / "frames.log"),
            "XDG_STATE_HOME": str(tmp_path / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert program.read_bytes() == b"A execution-peer fixture"
    assert guest_data.read_bytes() == b"saved game"
    preferences = (state_root / "preferences").read_text()
    recent_line = next(
        line for line in preferences.splitlines() if line.startswith("recent-documents=")
    )
    assert json.loads(recent_line.partition("=")[2].replace("\\\\", "\\")) == []


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
    (directory / "slow.vxp").write_bytes(b"W slow-check fixture")
    (directory / "failing.vxp").write_bytes(b"F failing-start fixture")
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
            "FPLINUX_APP_FRAME_RECORD": str(directory / "frames.log"),
            "XDG_STATE_HOME": str(directory / "state"),
        },
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "mre-ui: first modal document lifecycle checked\n"
    records = [line.split()[:2] for line in record.read_text().splitlines()]
    started = [program] + (["B"] if scenario == "replace-editor" else [])
    assert [entry for entry in records if entry[0] == "S"] == [["S", name] for name in started]
    if scenario in {"slow", "failed-start", "replace-editor"}:
        return
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

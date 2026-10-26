# SPDX-License-Identifier: GPL-2.0-only
"""Real Settings, UI runtime and Wayland frames with controlled session replies."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

SCREENS = ROOT / "site/scripts/screens"


@pytest.fixture(scope="module")
def settings_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path
) -> Iterator[Path]:
    """Build the real app and peer against the shared native UI inputs."""
    with c_build_directory(tmp_path_factory, "settings-native") as directory:
        run_process(
            ["sh", str(SCREENS / "build-settings.sh"), str(directory), str(ui_host_inputs)],
            name="build the actual Settings and native session peer",
            timeout=180,
            check=True,
        )
        yield directory


def test_actual_settings_applied_storage_errors_commit_before_ack_and_keep_notice(
    settings_native: Path, tmp_path: Path
) -> None:
    """A session peer injects storage outcomes; the actual app paints and handles input."""
    result = run_process(
        [
            str(settings_native / "settings-screens"),
            str(settings_native / "settings"),
            str(SCREENS / "keypad.xkb"),
            str(tmp_path / "session.sock"),
            str(tmp_path),
            str(ROOT / "alpine/aports/fplinux-language"),
        ],
        name="apply Settings theme and locale storage failures through actual UI frames",
        timeout=20,
    )
    (tmp_path / "capture.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    log_root = os.environ.get("FPLINUX_LOG_ROOT")
    if log_root:
        shutil.copytree(
            tmp_path,
            Path(log_root) / "settings-storage-frames",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == (
        "settings-warning: Cancel leaves the live theme and sends no change\n"
        "settings-success: live Light widgets and pixels precede REDRAW_ACK\n"
        "settings-theme-error: applied Dark frame acknowledges ENOSPC; notice waits for Close\n"
        "settings-date: real editors retain date, time and focus through pending Home, refusal "
        "and cancelled close; live-clock reply completes Save\n"
        "settings-locale-error: applied Русский frame acknowledges EIO; "
        "rebuilt notice waits for Закрыть\n"
    )
    frames = [
        (tmp_path / f"settings-{name}.rgb565").read_bytes()
        for name in ("theme-saved", "theme-error", "locale-error", "locale-closed")
    ]
    assert all(len(frame) == 240 * 268 * 2 for frame in frames)
    assert len(set(frames)) == 4

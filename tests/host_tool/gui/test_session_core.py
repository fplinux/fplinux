# SPDX-License-Identifier: GPL-2.0-only
"""Host session manager with real child peers and controlled compositor commits."""

import shutil
from pathlib import Path
from tempfile import TemporaryDirectory

import pytest

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-session"


@pytest.fixture(scope="module")
def session_core_harness(tmp_path_factory: pytest.TempPathFactory) -> Path:
    executable = tmp_path_factory.mktemp("session-core") / "session-core-test"
    json_flags = run_process(
        ["pkg-config", "--cflags", "--libs", "json-c"],
        name="resolve session protocol dependency",
        timeout=10,
        check=True,
    )
    run_process(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include/fplinux"),
            "-I",
            str(APORT),
            str(Path(__file__).with_name("fplinux-session-core-test.c")),
            str(APORT / "session-core.c"),
            str(APORT / "session-power.c"),
            str(APORT / "session-settings.c"),
            str(ROOT / "lib/fplinux/fplinux-settings-values.c"),
            str(APORT / "session-battery.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-session.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
            str(ROOT / "lib/fplinux/fplinux-keypad-light.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-desktop-entry.c"),
            "-Wl,--wrap=clock_settime",
            "-Wl,--wrap=time",
            "-Wl,--wrap=fsync",
            "-Wl,--wrap=write",
            *json_flags.stdout.split(),
            "-o",
            str(executable),
        ],
        name="compile session manager host harness",
        timeout=30,
        check=True,
    )
    return executable


@pytest.mark.parametrize(
    "scenario",
    [
        "startup-frame-before-ready",
        "home-retains-exit-saves",
        "shutdown-saves-bounded",
        "task-controls",
        "task-localized-title",
        "task-stale-lock",
        "end-prompt-minimize",
        "end-prompt-close",
        "end-prompt-red-close",
        "end-prompt-cancel",
        "end-prompt-stale-instance",
        "end-prompt-lock",
        "end-prompt-updates",
        "end-prompt-fullscreen",
        "task-view-red-home",
        "task-view-back-home",
        "task-view-query-home",
        "task-view-restart-home",
        "task-view-restart-catalogue",
        "task-view-activation-selection",
        "first-buffer-handoff",
        "ready-after-buffer",
        "fullscreen-launch-held",
        "appearance-launcher-first",
        "appearance-requester-first",
        "appearance-save-fails",
        "appearance-end-minimize",
        "appearance-end-close",
        "peer-authentication",
        "cancel-cleans-process",
        "key-routing",
        "input-language",
        "input-language-save-fails",
        "brightness-home",
        "brightness-crash",
        "wake-stays-dark-before-presentation",
        "wake-late-commit",
        "wake-integrated-buffers",
        "wake-fullscreen-buffer",
        "wake-output-reset",
        "wake-new-wake",
        "wake-launcher-restart",
        "keypad-light",
        "keypad-light-binary",
        "settings-errors-time",
        "battery-charger-wake-charged",
        "battery-charger-wake-low",
        "battery-charger-wake-critical",
    ],
)
def test_session_manager_host_behavior(session_core_harness: Path, scenario: str) -> None:
    # Unix pathname sockets require a short path independent of pytest's case name.
    with TemporaryDirectory(prefix="fplinux-session-") as scratch:
        if scenario == "task-localized-title":
            shutil.copyfile(
                ROOT / "alpine/aports/fplinux-language/ru", Path(scratch) / "system-language"
            )
        run_process(
            [str(session_core_harness), scenario, scratch],
            name=f"run session manager host scenario {scenario}",
            timeout=15,
            check=True,
        )


@pytest.mark.parametrize(
    ("width", "height", "top", "app_height"),
    [(128, 160, 16, 128), (240, 320, 20, 268), (320, 240, 20, 196)],
)
def test_session_layout_assigns_profile_surface_box(
    session_core_harness: Path, width: int, height: int, top: int, app_height: int
) -> None:
    with TemporaryDirectory(prefix="fplinux-session-") as scratch:
        run_process(
            [
                str(session_core_harness),
                "surface-box",
                scratch,
                str(width),
                str(height),
                str(top),
                str(app_height),
            ],
            name="run session surface placement host scenario",
            timeout=15,
            check=True,
        )

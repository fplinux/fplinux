# SPDX-License-Identifier: GPL-2.0-only
"""Host session manager with real child peers and controlled compositor commits."""

import shutil
from pathlib import Path
from tempfile import TemporaryDirectory

import pytest

from tests.c_harness import compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-session"


@pytest.fixture(scope="module")
def session_core_harness(tmp_path_factory: pytest.TempPathFactory) -> Path:
    executable = tmp_path_factory.mktemp("session-core") / "session-core-test"
    settings = ROOT / "alpine/aports/fplinux-settings"
    app_flags = [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(ROOT / "tests/host_tool/gui/settings-boundary"),
        "-I",
        str(ROOT / "include"),
        "-I",
        str(ROOT / "include/fplinux"),
        "-Dfplinux_language_load=settings_app_language_load",
        "-Dfplinux_language_list=settings_app_language_list",
        "-Dfplinux_language_english=settings_app_language_english",
    ]
    app_object = executable.with_name("settings-app.o")
    boundary_object = executable.with_name("settings-boundary.o")
    compile_c(
        app_object,
        [*app_flags, "-Dmain=fplinux_settings_main", "-c", str(settings / "fplinux-settings.c")],
        name="compile whole Settings application for manager integration",
    )
    compile_c(
        boundary_object,
        [
            *app_flags,
            "-Dmain=settings_app_harness_main",
            "-c",
            str(Path(__file__).with_name("settings_app.c")),
        ],
        name="compile controlled Settings screen boundary",
    )
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
            str(ROOT / "tests/host_tool/gui/settings-boundary"),
            "-I",
            str(ROOT / "include"),
            "-I",
            str(ROOT / "include/fplinux"),
            "-I",
            str(APORT),
            "-I",
            str(ROOT / "alpine/aports/fplinux-clocks"),
            str(Path(__file__).with_name("fplinux-session-core-test.c")),
            str(APORT / "session-core.c"),
            str(APORT / "session-audio.c"),
            str(APORT / "session-vibration.c"),
            str(APORT / "session-vibrator.c"),
            str(APORT / "session-power.c"),
            str(APORT / "session-settings.c"),
            str(ROOT / "lib/fplinux/fplinux-settings-values.c"),
            str(APORT / "session-battery.c"),
            str(APORT / "session-clock.c"),
            str(ROOT / "alpine/aports/fplinux-clocks/clock-model.c"),
            str(ROOT / "alpine/aports/fplinux-clocks/clock-wire.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-session.c"),
            str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
            str(ROOT / "lib/fplinux/fplinux-brightness-client.c"),
            str(ROOT / "lib/fplinux/fplinux-keypad-light.c"),
            str(ROOT / "lib/fplinux/fplinux-language.c"),
            str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            str(ROOT / "lib/fplinux/fplinux-desktop-entry.c"),
            str(settings / "settings-core.c"),
            str(ROOT / "lib/fplinux/fplinux-multitap.c"),
            str(ROOT / "lib/fplinux/fplinux-cli.c"),
            str(app_object),
            str(boundary_object),
            *gettext_link_flags(),
            "-Wl,--wrap=clock_settime",
            "-Wl,--wrap=time",
            "-Wl,--wrap=fopen",
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
        "media-home-resume-close",
        "media-lock-crash",
        "media-effect-error",
        "media-clock-owner",
        "clock-survives-home-close",
        "clock-reset-rtc",
        "clock-signal-lifetime",
        "clock-launcher-restart",
        "clock-foreign-peer",
        "clock-launcher-authority",
        "clock-locked-ring-keys",
        "clock-save-retry",
        "clock-published-retry",
        "clock-save-current-model",
        "startup-frame-before-ready",
        "home-retains-exit-saves",
        "close-cancel-stale",
        "close-ready-scenes",
        "close-wait-scenes",
        "close-wait-home-repeat",
        "close-unanswered-timeout",
        "close-wait-after-home",
        "close-background-wait",
        "settings-retry-latest",
        "settings-retry-unchanged",
        "settings-applied-error-view",
        "settings-preview-cancel-retry",
        "settings-preview-cancel-published-retry",
        "shutdown-saves-bounded",
        "task-controls",
        "task-localized-title",
        "activation-intent-frame-barrier",
        "activation-ended-task",
        "activation-cancel-retains",
        "activation-home-retains",
        "activation-timeout-retains",
        "task-localized-live",
        "task-localized-live-save-fails",
        "task-localized-explicit-default",
        "task-stale-lock",
        "red-home-retains",
        "red-home-fullscreen",
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
        "appearance-home",
        "appearance-close",
        "close-appearance-independent",
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
        "settings-theme-home-before-request",
        "settings-locale-home-before-request",
        "battery-notice-frame",
        "battery-notice-upgrade",
        "battery-charger-wake-charged",
        "battery-charger-wake-low",
        "battery-charger-wake-critical",
    ],
)
def test_session_manager_host_behavior(session_core_harness: Path, scenario: str) -> None:
    # Unix pathname sockets require a short path independent of pytest's case name.
    with TemporaryDirectory(prefix="fplinux-session-") as scratch:
        if scenario.startswith("task-localized-"):
            shutil.copyfile(
                ROOT / "alpine/aports/fplinux-language/ru", Path(scratch) / "system-language"
            )
        if scenario in {
            "task-localized-live",
            "task-localized-live-save-fails",
            "task-localized-explicit-default",
        }:
            shutil.copyfile(
                ROOT / "alpine/aports/fplinux-language/en", Path(scratch) / "system-language-en"
            )
        result = run_process(
            [str(session_core_harness), scenario, scratch],
            name=f"run session manager host scenario {scenario}",
            timeout=15,
            check=True,
        )
        if scenario in {"settings-retry-latest", "settings-retry-unchanged"}:
            assert result.stderr.count("fplinux-session: cannot save ") == 1
            assert result.stderr.count("fplinux-session: settings saved: ") == 1


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

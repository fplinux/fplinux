# SPDX-License-Identifier: GPL-2.0-only
"""Actual UI frames, input and media lifecycle through controlled service peers."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture(scope="module")
def client_frames(
    tmp_path_factory: pytest.TempPathFactory, lvgl_library: tuple[Path, Path]
) -> Path:
    """Link the actual UI library and renderer against real Wayland libraries."""
    directory = tmp_path_factory.mktemp("ui-client-frames")
    protocol = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
    for kind, filename in (
        ("client-header", "xdg-shell-client-protocol.h"),
        ("server-header", "xdg-shell-server-protocol.h"),
        ("private-code", "xdg-shell-protocol.c"),
    ):
        run_process(
            ["wayland-scanner", kind, protocol, str(directory / filename)],
            name=f"generate the test xdg-shell {kind}",
            timeout=10,
            check=True,
        )
    headers, archive = lvgl_library
    flags = run_process(
        [
            "pkg-config",
            "--cflags",
            "--libs",
            "wayland-client",
            "wayland-server",
            "xkbcommon",
            "json-c",
        ],
        name="resolve the actual client and compositor libraries",
        timeout=10,
        check=True,
    ).stdout.split()
    executable = directory / "client-frames"
    sources = [
        "fplinux-ui.c",
        "fplinux-ui-style.c",
        "fplinux-audio-client.c",
        "fplinux-ui-frame.c",
        "fplinux-ui-prefs.c",
        "fplinux-ui-session.c",
        "fplinux-ui-text.c",
        "fplinux-ui-text-model.c",
        "fplinux-ui-symbol-picker.c",
        "fplinux-text-input.c",
        "fplinux-ui-menu.c",
        "fplinux-ui-screen.c",
        "fplinux-ui-screen-model.c",
        "fplinux-ui-listview.c",
        "fplinux-ui-locale.c",
        "fplinux-ui-profile.c",
        "fplinux-ui-softkey-layout.c",
        "fplinux-multitap.c",
        "fplinux-language.c",
        "fplinux-utf8.c",
    ]
    compile_c(
        executable,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-isystem",
            str(headers),
            "-DLV_KCONFIG_IGNORE",
            "-DLV_LVGL_H_INCLUDE_SYSTEM",
            f'-DLV_CONF_PATH="{ROOT / "alpine/aports/fplinux-lvgl/lv_conf.h"}"',
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{ROOT / 'site/scripts/screens'}",
            f"-I{directory}",
            str(ROOT / "tests/host_tool/gui/ui-client-frames.c"),
            str(ROOT / "site/scripts/screens/ui-frame-peer.c"),
            *(str(ROOT / "lib/fplinux" / source) for source in sources),
            str(directory / "xdg-shell-protocol.c"),
            str(archive),
            *flags,
            *gettext_link_flags(),
            "-lm",
        ],
        name="link the complete UI client and test compositor",
    )
    return executable


def test_successful_exit_retains_frame_until_manager_close(
    client_frames: Path, tmp_path: Path
) -> None:
    """A successful Back retains visible pixels and waits for manager CLOSE."""
    result = run_process(
        [str(client_frames), "exit", str(tmp_path / "session.sock")],
        name="retain the last client SHM frame while exiting",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("exit: ok\n")


def test_failed_exit_send_closes_client_without_waiting_for_manager(
    client_frames: Path, tmp_path: Path
) -> None:
    """A lost manager connection makes Back fail and closes the client promptly."""
    result = run_process(
        [str(client_frames), "disconnect", str(tmp_path / "session.sock")],
        name="close the client when the manager disappears during Back",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("disconnect: ok\n")


def test_canvas_softkeys_keep_full_bar_alignment_without_redundant_frame(
    client_frames: Path, tmp_path: Path
) -> None:
    """The launcher-style real LVGL bar centres labels and leaves identical updates quiet."""
    result = run_process(
        [str(client_frames), "softkeys", str(tmp_path / "session.sock")],
        name="align Canvas legends and avoid a redundant client SHM frame",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("softkeys: ok\n")


def test_media_capabilities_reject_requests_before_visibility_guards(
    client_frames: Path, tmp_path: Path
) -> None:
    """Real CONFIG without media keeps EINVAL and ENOTSUP while visible or hidden."""
    result = run_process(
        [str(client_frames), "media-unsupported", str(tmp_path / "session.sock")],
        name="honour configured media capabilities in the actual UI runtime",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("media-unsupported: ok\n")


def test_media_lifecycle_updates_frame_timers_before_callbacks_and_sends_vibration(
    client_frames: Path, tmp_path: Path
) -> None:
    """The actual runtime owns hide/show, leaving, frame cleanup and SESSION delivery."""
    result = run_process(
        [str(client_frames), "media-lifecycle", str(tmp_path / "session.sock")],
        name="apply media and SoftwareFrame lifecycle through real UI session packets",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("media-lifecycle: ok\n")


def assert_media_audio_scenario(client_frames: Path, tmp_path: Path, scenario: str) -> None:
    result = run_process(
        [
            str(client_frames),
            f"media-audio-{scenario}",
            str(tmp_path / "session.sock"),
            str(tmp_path / "audio.sock"),
        ],
        name=f"observe UI audio stream {scenario} through real transport and shared memory",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"media-audio-{scenario}: ok\n")


def test_ui_close_releases_every_audio_stream_left_open_by_the_application(
    client_frames: Path, tmp_path: Path
) -> None:
    """The actual UI closes two real mapped streams and disconnects from the audio peer."""
    assert_media_audio_scenario(client_frames, tmp_path, "close")


def test_ui_runtime_dispatches_audio_service_hup_before_the_next_application_write(
    client_frames: Path, tmp_path: Path
) -> None:
    """A real socket HUP ends a mapped stream whose shared state still says PLAYING."""
    assert_media_audio_scenario(client_frames, tmp_path, "hup")


@pytest.mark.parametrize(
    "scenario",
    ["active", "setting", "locale", "error", "applied-theme", "applied-locale", "unapplied-theme"],
)
def test_acknowledgement_waits_for_complete_new_frame(
    client_frames: Path, tmp_path: Path, scenario: str
) -> None:
    """Busy SHM buffers defer ACK until a complete callback redraw is committed."""
    result = run_process(
        [str(client_frames), scenario, str(tmp_path / "session.sock")],
        name=f"acknowledge the fully committed client {scenario} frame",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"{scenario}: ok\n")


def test_ordinary_close_preparation_waits_for_final_manager_close(
    client_frames: Path, tmp_path: Path
) -> None:
    """A ready client answers the attempt and keeps running until final CLOSE."""
    result = run_process(
        [str(client_frames), "prepare-close", str(tmp_path / "session.sock")],
        name="answer ordinary close preparation without leaving early",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("prepare-close: ok\n")


def test_opening_preferences_does_not_create_the_application_state_directory(
    client_frames: Path, tmp_path: Path
) -> None:
    """Opening missing preferences is read-only until a real save is requested."""
    result = run_process(
        [str(client_frames), "prefs-lazy", str(tmp_path / "absent-state")],
        name="open missing UI preferences without filesystem mutations",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("prefs-lazy: ok\n")


@pytest.mark.parametrize("scenario", ["close-decisions", "close-forced"])
def test_close_handler_keeps_wait_interactive_and_final_close_bounded(
    client_frames: Path, tmp_path: Path, scenario: str
) -> None:
    """Back can be cancelled/retried; forced CLOSE also ends a waiting decision."""
    result = run_process(
        [str(client_frames), scenario, str(tmp_path / "session.sock")],
        name=f"exercise the actual UI close handler: {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"{scenario}: ok\n")


def test_storage_errors_distinguish_capacity_access_and_io_failures(
    client_frames: Path, tmp_path: Path
) -> None:
    """Shared presentation preserves errno causes before and after publication."""
    result = run_process(
        [str(client_frames), "storage-errors", str(tmp_path / "unused")],
        name="present shared UI storage failures by their actual result",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith("storage-errors: ok\n")


def assert_repeat_scenario(client_frames: Path, tmp_path: Path, scenario: str) -> None:
    result = run_process(
        [
            str(client_frames),
            f"repeat-{scenario}",
            str(tmp_path / "session.sock"),
            str(ROOT / "tests/host_tool/gui/repeat-keymap.xkb"),
        ],
        name=f"exercise real Wayland UI repeat: {scenario}",
        timeout=10,
        check=True,
    )
    assert result.stdout.endswith(f"repeat-{scenario}: ok\n")


@pytest.mark.parametrize("scenario", ["list", "form", "textbox", "alert", "menu"])
def test_standard_navigation_repeats_one_wayland_hold_and_stops_on_release(
    client_frames: Path, tmp_path: Path, scenario: str
) -> None:
    """Real controls keep moving after one arrow press and stop after its release."""
    assert_repeat_scenario(client_frames, tmp_path, scenario)


def test_opted_in_canvas_moves_a_real_lvgl_textarea_during_one_wayland_hold(
    client_frames: Path, tmp_path: Path
) -> None:
    """A Canvas callback retains its explicit repeat policy for a native textarea."""
    assert_repeat_scenario(client_frames, tmp_path, "lvgl-textarea")


def test_canvas_navigation_policy_repeats_arrows_and_keeps_enter_and_tab_single(
    client_frames: Path, tmp_path: Path
) -> None:
    """Repeatable XKB Enter and Tab invoke one action while a held arrow keeps moving."""
    assert_repeat_scenario(client_frames, tmp_path, "navigation-policy")


@pytest.mark.parametrize("scenario", ["release", "blur", "delete", "menu"])
def test_raw_field_without_canvas_repeats_through_application_and_cancels_hold(
    client_frames: Path, tmp_path: Path, scenario: str
) -> None:
    """The void application callback moves a real field and ends its captured hold."""
    assert_repeat_scenario(client_frames, tmp_path, f"raw-{scenario}")


def test_raw_field_rejects_repeat_claims_for_application_created_keys(
    client_frames: Path, tmp_path: Path
) -> None:
    """Calling text_key outside a Wayland dispatch moves once without arming a hold."""
    assert_repeat_scenario(client_frames, tmp_path, "raw-synthetic")


def test_raw_field_rejects_a_copy_of_the_current_wayland_key(
    client_frames: Path, tmp_path: Path
) -> None:
    """Equal key values cannot claim a hold without the actual dispatch key object."""
    assert_repeat_scenario(client_frames, tmp_path, "raw-copy")


def test_raw_field_blur_during_the_initial_change_callback_revokes_pending_hold(
    client_frames: Path, tmp_path: Path
) -> None:
    """Returning focus before text_key returns cannot revive its cancelled claim."""
    assert_repeat_scenario(client_frames, tmp_path, "raw-pending-blur")


def test_canvas_repeat_requires_opt_in_and_resolves_current_xkb_state(
    client_frames: Path, tmp_path: Path
) -> None:
    """Raw handling alone is quiet; enabled repeat observes fresh modifiers and layout."""
    assert_repeat_scenario(client_frames, tmp_path, "keyboard")


def test_phone_canvas_repeat_accepts_only_direction_keys(
    client_frames: Path, tmp_path: Path
) -> None:
    """Arrows repeat while digits, star, pound, OK and both softkeys stay single presses."""
    assert_repeat_scenario(client_frames, tmp_path, "phone")


def test_canvas_repeat_does_not_catch_up_after_a_blocked_application_callback(
    client_frames: Path, tmp_path: Path
) -> None:
    """A controlled pause leaves no burst of overdue key callbacks."""
    assert_repeat_scenario(client_frames, tmp_path, "pause")


def test_raw_text_input_cancels_overdue_phone_gestures_before_synthetic_release(
    client_frames: Path, tmp_path: Path
) -> None:
    """Wayland leave keeps the letter/phone zero and preserves field-anchored Symbols."""
    assert_repeat_scenario(client_frames, tmp_path, "text-cancel")


@pytest.mark.parametrize(
    "scenario",
    [
        "leave",
        "visibility",
        "options",
        "stack",
        "bounce",
        "replace",
        "disable",
        "policy",
        "keymap",
        "seat",
        "rate-zero",
        "home",
        "close",
    ],
)
def test_canvas_repeat_cancellation_never_revives_the_old_wayland_hold(
    client_frames: Path, tmp_path: Path, scenario: str
) -> None:
    """Focus, modal, lifetime and compositor changes stop the actual held key."""
    assert_repeat_scenario(client_frames, tmp_path, scenario)

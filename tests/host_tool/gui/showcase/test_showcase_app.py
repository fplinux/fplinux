# SPDX-License-Identifier: GPL-2.0-only
"""Actual App, software scene, UI and preferences with Session/Wayland peers.

Only the GPU, system identity files and clocks are controlled link
boundaries. Presentation permits make the real event loop observable; they do not
acknowledge physical presentation or qualify phone hardware.
"""

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

HERE = ROOT / "tests/host_tool/gui/showcase"


@pytest.fixture(scope="module")
def showcase_native(
    tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path, ui_icon_resources: Path
) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "showcase-native") as directory:
        run_process(
            ["sh", str(HERE / "native/build.sh"), str(ROOT), str(directory), str(ui_host_inputs)],
            name="build actual Showcase, software scene and native UI peer",
            timeout=180,
            check=True,
        )
        (directory / "icons").symlink_to(ui_icon_resources, target_is_directory=True)
        yield directory


@pytest.mark.parametrize(
    ("arguments", "status"),
    [
        (["--help", "--runs=bad", "--renderer=invalid"], 0),
        (["--runs=1"], 2),
        (["--renderer=software"], 2),
        (["--keypad-led=/missing"], 2),
        (["unexpected"], 2),
    ],
)
def test_showcase_cli_finishes_before_opening_ui(
    showcase_native: Path, arguments: list[str], status: int
) -> None:
    result = run_process(
        [str(showcase_native / "showcase"), *arguments],
        name="parse Showcase help or unsupported syntax without Session",
        env={"FPLINUX_GUI_SESSION": "/nonexistent/showcase-session.sock"},
        timeout=5,
    )
    assert result.returncode == status
    assert "fplinux_ui_open" not in result.stderr
    if status == 0:
        assert "Usage:" in result.stdout
        assert "--help" in result.stdout
    else:
        assert result.stderr


@pytest.mark.parametrize("screen", [(128, 128), (128, 160), (176, 220), (240, 320), (320, 240)])
def test_setup_waits_for_start_and_configure_then_uses_full_and_standard_viewport(
    showcase_native: Path, tmp_path: Path, screen: tuple[int, int]
) -> None:
    run_native(showcase_native, tmp_path, screen, scenario="geometry")


def test_stop_counts_generated_frames_and_active_time_including_submillisecond_renders(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 128), scenario="measurement")


@pytest.mark.parametrize(
    ("screen", "theme", "locale"),
    [((128, 128), "light", "en"), ((240, 320), "dark", "ru")],
)
def test_default_run_opens_the_same_completed_result_in_each_language(
    showcase_native: Path, tmp_path: Path, screen: tuple[int, int], theme: str, locale: str
) -> None:
    run_native(showcase_native, tmp_path, screen, scenario="localized", theme=theme, locale=locale)


@pytest.mark.parametrize("cycles", ["1", "2", "3", "5", "10", "unlimited"])
def test_cycle_choice_preserves_selection_and_uses_38_seconds_of_scene_time(
    showcase_native: Path, tmp_path: Path, cycles: str
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario=f"cycles-{cycles}")


@pytest.mark.parametrize(
    "scenario", ["interrupt-visibility", "interrupt-lifecycle", "interrupt-resize"]
)
def test_running_interruption_retains_partial_measurement_and_never_resumes(
    showcase_native: Path, tmp_path: Path, scenario: str
) -> None:
    run_native(showcase_native, tmp_path, (176, 220), scenario=scenario)


def test_explicit_gpu_unavailable_keeps_setup_and_auto_fallback_repeats_actual_software(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="gpu-unavailable")


def test_gpu_render_error_retains_result_and_repeat_uses_the_actual_gpu(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="gpu-failure")


def test_stop_while_waiting_for_fullscreen_produces_zero_frame_result(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="preparation-stop")


@pytest.mark.parametrize("scenario", ["effects", "effects-subset"])
def test_effect_selection_obeys_capabilities_and_stop_releases_system_overrides(
    showcase_native: Path, tmp_path: Path, scenario: str
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario=scenario)
    if scenario == "effects":
        run_native(showcase_native, tmp_path, (128, 160), scenario="restored-effects")


def test_disabled_effects_send_no_light_or_vibration_requests(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="effects-off")


@pytest.mark.parametrize("scenario", ["effects-interrupted", "effects-close"])
def test_interruption_and_close_release_effects_and_close_does_not_push_result(
    showcase_native: Path, tmp_path: Path, scenario: str
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario=scenario)


def test_unavailable_selected_effect_ends_the_run_with_error(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="effects-unavailable")


def test_phone_ok_starts_and_open_resets_setup_while_resume_preserves_the_existing_view(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="activation")


def test_single_saved_configuration_reopens_setup_without_scene_effects_or_result(
    showcase_native: Path, tmp_path: Path
) -> None:
    run_native(showcase_native, tmp_path, (128, 160), scenario="persist-settings")
    preferences = tmp_path / "state/fplinux/org.fplinux.showcase/preferences"
    assert preferences.is_file()
    assert dict(line.split("=", 1) for line in preferences.read_text().splitlines()) == {
        "renderer": "1",
        "fullscreen": "0",
        "cycles": "0",
        "effects": "7",
    }
    run_native(showcase_native, tmp_path, (128, 160), scenario="restored-settings")


def test_obsolete_prefixed_preferences_do_not_change_the_single_configuration_defaults(
    showcase_native: Path, tmp_path: Path
) -> None:
    preferences = tmp_path / "state/fplinux/org.fplinux.showcase/preferences"
    preferences.parent.mkdir(parents=True, mode=0o700)
    preferences.write_text(
        "preset=2\ndemo.renderer=2\ndemo.fullscreen=0\ndemo.cycles=10\n"
        "benchmark.renderer=2\nstress.renderer=2\nstress.fullscreen=0\nstress.cycles=0\n",
        encoding="utf-8",
    )
    preferences.chmod(0o600)
    run_native(showcase_native, tmp_path, (128, 160), scenario="default-settings")


def run_native(  # noqa: PLR0913 -- independent display, language and scenario controls.
    directory: Path,
    work: Path,
    screen: tuple[int, int],
    *,
    scenario: str,
    theme: str = "light",
    locale: str = "en",
) -> None:
    width, height = screen
    (work / "os-release").write_text(
        'NAME="Component test OS"\nBUILD_ID="component-test-build"\n', encoding="utf-8"
    )
    (work / "model").write_bytes(b"Component test model\0")
    result = run_process(
        [
            str(directory / "showcase-screens"),
            str(directory / "showcase"),
            str(ROOT / "tests/host_tool/gui/screens/keypad.xkb"),
            str(work / "session.sock"),
            str(work / "initial.rgb565"),
            str(width),
            str(height),
            theme,
            locale,
            scenario,
        ],
        name=f"actual Showcase {scenario} at {width}x{height} ({theme}, {locale})",
        env={
            "FPLINUX_NATIVE_LANGUAGE_DIR": str(ROOT / "alpine/aports/fplinux-language"),
            "FPLINUX_SHOWCASE_OS_RELEASE": str(work / "os-release"),
            "FPLINUX_SHOWCASE_MODEL": str(work / "model"),
            "XDG_STATE_HOME": str(work / "state"),
            "FPLINUX_NATIVE_GLYPHS": str(directory / "icons/usr/share/fplinux/glyphs"),
        },
        timeout=90,
    )
    (work / f"{scenario}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if log_root := os.environ.get("FPLINUX_LOG_ROOT"):
        shutil.copytree(
            work,
            Path(log_root) / f"showcase-{scenario}-{width}x{height}-{theme}-{locale}",
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("session.sock"),
        )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.endswith(
        "showcase: actual App setup, software scene, controlled Session and Wayland\n"
    )
    frame = (work / "initial.rgb565").read_bytes()
    assert len(frame) % (width * 2) == 0
    assert 0 < len(frame) // (width * 2) < height
    assert len(set(frame)) > 2

# SPDX-License-Identifier: GPL-2.0-only
"""Settings host checks with controlled UI, session, timer and language-store boundaries."""

from __future__ import annotations

import gettext
import platform
import re
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture(scope="module")
def settings_app(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the whole application and multi-tap loader as separate objects."""
    aport = ROOT / "alpine/aports/fplinux-settings"
    with c_build_directory(tmp_path_factory, "settings-app") as directory:
        flags = [
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
        ]
        app_object = directory / "settings-app.o"
        compile_c(
            app_object,
            [*flags, "-Dmain=fplinux_settings_main", "-c", str(aport / "fplinux-settings.c")],
            name="compile whole Settings application",
        )
        executable = directory / "settings-app"
        compile_c(
            executable,
            [
                *flags,
                str(ROOT / "tests/host_tool/gui/settings_app.c"),
                str(aport / "settings-core.c"),
                str(ROOT / "lib/fplinux/fplinux-settings-values.c"),
                str(ROOT / "lib/fplinux/fplinux-multitap.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-profile.c"),
                str(app_object),
                *gettext_link_flags(),
            ],
            name="link Settings application harness",
        )
        yield executable


@pytest.mark.parametrize(
    ("profile", "logo"),
    [
        (0, "mark-{theme}-32.png"),
        (1, "mark-{theme}-32.png"),
        (2, "lockup-{theme}-160.png"),
        (3, "lockup-{theme}-200.png"),
        (4, "lockup-{theme}-200.png"),
    ],
)
@pytest.mark.parametrize("theme", ["light", "dark"])
def test_settings_about_shows_runtime_identity_and_profile_logo_then_returns(
    settings_app: Path, profile: int, logo: str, theme: str
) -> None:
    """Whole-app callbacks use the public form/image API and native host facts."""
    result = run_process(
        [
            str(settings_app),
            "about",
            str(profile),
            theme,
            f"/usr/share/fplinux/brand/{logo.format(theme=theme)}",
        ],
        name=f"Settings About for profile {profile} and {theme} theme",
        timeout=10,
        check=True,
    )
    assert_about_runtime_facts(result.stdout, "about")
    assert result.stderr == ""


def assert_about_runtime_facts(output: str, scenario: str) -> None:
    """Compare displayed facts with independent host interfaces, without fixed versions."""
    lines = output.splitlines()
    assert lines[-1] == f"settings-app: {scenario} passed"
    facts = dict(line.split(": ", 1) for line in lines[:-1])
    system = platform.freedesktop_os_release()
    version = system.get("VERSION") or system.get("VERSION_ID")
    assert facts["System"] == system["NAME"] + (f" {version}" if version else "")
    assert facts["Kernel"] == platform.release()
    alpine_release = Path("/etc/alpine-release")
    if alpine_release.exists():
        assert facts["Alpine Linux"] == alpine_release.read_text().splitlines()[0]
    else:
        assert "Alpine Linux" not in facts
    memory = re.fullmatch(
        r"([0-9]+\.[0-9]) / ([0-9]+\.[0-9])\N{NO-BREAK SPACE}MiB", facts["Free memory"]
    )
    assert memory is not None
    available, total = map(float, memory.groups())
    assert 0 <= available <= total
    total_line = next(
        line
        for line in Path("/proc/meminfo").read_text().splitlines()
        if line.startswith("MemTotal:")
    )
    total_kib = int(total_line.split()[1])
    assert total == float(f"{total_kib / 1024:.1f}")


def test_settings_about_keeps_runtime_facts_and_back_when_logo_is_missing(
    settings_app: Path,
) -> None:
    logo = "/usr/share/fplinux/brand/mark-light-32.png"
    result = run_process(
        [str(settings_app), "about-missing-logo", "0", "light", logo],
        name="Settings About without an installed logo",
        timeout=10,
        check=True,
    )
    assert_about_runtime_facts(result.stdout, "about-missing-logo")
    assert result.stderr.startswith(f"fplinux-settings: {logo}: ")


def test_settings_russian_catalogue_translates_about_labels(tmp_path: Path) -> None:
    """The compiled installed catalogue supplies the About page's Russian labels."""
    catalogue = tmp_path / "ru.mo"
    run_process(
        [
            "msgfmt",
            "--check",
            "--output-file",
            str(catalogue),
            str(ROOT / "alpine/aports/fplinux-settings/ru.po"),
        ],
        name="compile Settings Russian catalogue",
        timeout=30,
        check=True,
    )
    with catalogue.open("rb") as file:
        russian = gettext.GNUTranslations(file)
    assert russian.gettext("About") == "О системе"  # noqa: RUF001 -- intentional Russian preposition.
    assert russian.gettext("System") == "Система"
    assert russian.gettext("Kernel") == "Ядро"
    assert russian.gettext("Free memory") == "Свободная память"
    assert russian.gettext("%s / %s\N{NO-BREAK SPACE}MiB") == "%s / %s\N{NO-BREAK SPACE}МиБ"


@pytest.mark.parametrize(
    "scenario",
    [
        "errors",
        "preview-failure",
        "cancel",
        "save",
        "input",
        "theme-save-failed",
        "locale-save-failed",
        "theme-applied",
        "locale-applied",
        "theme-failed",
        "locale-failed",
    ],
)
def test_settings_callbacks_preserve_visible_values_and_setting_requests(
    settings_app: Path, scenario: str
) -> None:
    """Application callbacks produce labels, gauge state and session requests."""
    result = run_process(
        [str(settings_app), scenario], name=f"run Settings {scenario}", timeout=10, check=True
    )
    assert result.stdout == f"settings-app: {scenario} passed\n"


@pytest.mark.parametrize("finish", ["save", "cancel"])
def test_settings_background_discards_brightness_preview_before_resume(
    settings_app: Path, finish: str
) -> None:
    """A retained task returns to the stored gauge and cannot save a stale preview."""
    scenario = f"preview-background-{finish}"
    result = run_process(
        [str(settings_app), scenario], name=f"run Settings {scenario}", timeout=10, check=True
    )
    assert result.stdout == f"settings-app: {scenario} passed\n"
    assert result.stderr == ""


def test_settings_visibility_only_preserves_foreground_brightness_preview(
    settings_app: Path,
) -> None:
    """Temporary hiding changes visibility without ending the foreground edit."""
    result = run_process(
        [str(settings_app), "preview-hidden"],
        name="run Settings hidden foreground preview",
        timeout=10,
        check=True,
    )
    assert result.stdout == "settings-app: preview-hidden passed\n"
    assert result.stderr == ""


@pytest.mark.parametrize("setting", ["theme", "locale"])
@pytest.mark.parametrize("outcome", ["applied", "failed", "save-failed"])
def test_settings_pending_visual_choice_holds_back_until_reply_finishes(
    settings_app: Path, setting: str, outcome: str
) -> None:
    """Queued Back keys cannot depart a selector while its visual change is pending."""
    scenario = f"{setting}-pending-back-{outcome}"
    result = run_process(
        [str(settings_app), scenario], name=f"run Settings {scenario}", timeout=10, check=True
    )
    assert result.stdout == f"settings-app: {scenario} passed\n"
    if outcome == "applied":
        assert result.stderr == ""
    else:
        value = "dark" if setting == "theme" else "fr"
        assert result.stderr == f'fplinux-settings: {setting}="{value}" failed: {outcome}\n'


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_settings_help_precedes_arguments_and_resource_access(
    settings_app: Path, arguments: list[str]
) -> None:
    result = run_process([str(settings_app), "cli", *arguments], name="Settings help", timeout=10)
    assert result.returncode == 0
    assert "Usage: fplinux-settings" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("argument", ["--unknown", "unexpected"])
def test_settings_rejects_arguments_before_resource_access(
    settings_app: Path, argument: str
) -> None:
    result = run_process(
        [str(settings_app), "cli", argument], name="Settings arguments", timeout=10
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-settings:" in result.stderr

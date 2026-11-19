# SPDX-License-Identifier: GPL-2.0-only
"""Real phoneME document shell with controlled UI and VM boundaries, without Java execution."""

from __future__ import annotations

import base64
import errno
import os
import zipfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.gettext_support import gettext_link_flags
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

PORT = ROOT / "alpine/aports/fplinux-phoneme"
HERE = ROOT / "tests/host_tool/phoneme"


@pytest.fixture(scope="module")
def phoneme_app(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    """Link the shell, LF natives, scheduler, parser and JAR reader as separate units."""
    with c_build_directory(tmp_path_factory, "phoneme-app") as directory:
        executable = directory / "phoneme-app"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D_POSIX_C_SOURCE=200809L",
                f"-I{HERE / 'compat'}",
                f"-I{ROOT / 'include'}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{PORT / 'port/app'}",
                f"-I{PORT / 'vm'}",
                str(HERE / "app-boundary.c"),
                str(PORT / "port/app/fplinux-pm-app.c"),
                str(PORT / "port/app/fplinux-pm-lifecycle.c"),
                str(PORT / "port/app/fplinux-pm-store.c"),
                str(PORT / "port/lf-screens/fplinux-pm-lf.c"),
                str(PORT / "port/lf-screens/fplinux-pm-lf-trace.c"),
                str(PORT / "vm/fplinux-pm-jar.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-cli.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-prefs.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-recent.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-view-settings.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                *gettext_link_flags(),
                "-Wl,--wrap=fsync",
                "-Wl,--wrap=fplinux_ui_state_dir",
                "-Wl,--wrap=fplinux_ui_prefs_open",
                "-lz",
                "-lcrypto",
                "-ljson-c",
            ],
            name="compile actual phoneME document shell with controlled UI and VM",
        )
        yield executable


def write_suite(
    path: Path, *, multiple: bool = False, icon: bool = False, missing_class: bool = False
) -> None:
    """Create a normal ZIP with MIDP manifest fields and literal class entries."""
    icon_name = "icon.png" if icon else ""
    manifest = f"MIDlet-Name: Example suite\r\nMIDlet-1: First, {icon_name}, example.First\r\n"
    if multiple:
        manifest += "MIDlet-2: Second, , example.Second\r\n"
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("META-INF/MANIFEST.MF", manifest)
        if not missing_class or multiple:
            archive.writestr("example/First.class", b"\xca\xfe\xba\xbe")
        if icon:
            archive.writestr(
                "icon.png",
                base64.b64decode(
                    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8"
                    "/x8AAwMCAO+jRZkAAAAASUVORK5CYII="
                ),
            )
        if multiple and not missing_class:
            archive.writestr("example/Second.class", b"\xca\xfe\xba\xbe")


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_help_precedes_ui_and_storage(phoneme_app: Path, arguments: list[str]) -> None:
    result = run_process(
        [str(phoneme_app), *arguments], name="phoneME help before resources", timeout=10
    )
    assert result.returncode == 0
    assert "Usage: fplinux-phoneme" in result.stdout
    assert "Run a MIDlet JAR" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("argument", ["--unknown", "/games/example.jar"])
def test_cli_rejects_options_and_argv_document_routing(phoneme_app: Path, argument: str) -> None:
    result = run_process(
        [str(phoneme_app), argument], name="phoneME CLI error before UI", timeout=10
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-phoneme:" in result.stderr


def test_startup_offers_home_without_card_or_vm(phoneme_app: Path, tmp_path: Path) -> None:
    result = run_process(
        [str(phoneme_app)],
        name="phoneME empty startup",
        env=os.environ | {"FPLINUX_PM_TEST_ROOT": str(tmp_path)},
        timeout=10,
    )
    assert result.returncode == 0
    assert result.stdout == "home offers JAR, recent and settings; no VM started\n"
    assert result.stderr == ""


def test_empty_recent_has_no_remove_action(phoneme_app: Path, tmp_path: Path) -> None:
    result = run_process(
        [str(phoneme_app)],
        name="phoneME empty Recent",
        env=os.environ
        | {"FPLINUX_PM_TEST_SCENARIO": "recent-empty", "FPLINUX_PM_TEST_ROOT": str(tmp_path)},
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "empty Recent has no Remove action\n"


def test_removing_last_recent_entry_removes_action_and_preserves_data(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    original = jar.read_bytes()
    result = run_process(
        [str(phoneme_app)],
        name="phoneME remove last Recent entry",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": "recent-remove-last",
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert lines[-1] == "removing last Recent entry removes its action"
    storage = [
        Path(line.removeprefix("storage ")) for line in lines if line.startswith("storage ")
    ]
    assert len(storage) == 1
    assert (storage[0] / "record").read_text(encoding="utf-8") == "saved data"
    assert jar.read_bytes() == original
    assert "recent-documents=[]\n" in (tmp_path / "preferences").read_text(encoding="utf-8")


def test_picker_waits_for_storage_snapshot(phoneme_app: Path, tmp_path: Path) -> None:
    result = run_process(
        [str(phoneme_app)],
        name="phoneME picker waits for storage snapshot",
        env=os.environ
        | {"FPLINUX_PM_TEST_SCENARIO": "storage-picker", "FPLINUX_PM_TEST_ROOT": str(tmp_path)},
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "picker waits for the storage snapshot\n"


@pytest.mark.parametrize(
    ("scenario", "expected"),
    [
        ("single", "first MIDlet screen accepts document; one VM started"),
        ("picker-single", "picker routes JAR to the document handler; one VM started"),
        ("chooser-start", "first MIDlet screen accepts document; one VM started"),
        ("chooser-cancel", "chooser cancelled; no VM started"),
        ("start-failure", "failed VM start reports error before acceptance"),
        ("early-exit", "successful MIDlet exit accepts document before the first screen"),
        ("early-error", "MIDlet startup error reports document failure"),
        ("replace-busy", "refused replace preserves peer and pending document; retry accepts"),
        ("replace-missing", "refused replace preserves peer and pending document; retry accepts"),
        ("push-busy", "refused push preserves peer and pending document; retry accepts"),
        ("push-missing", "refused push preserves peer and pending document; retry accepts"),
        ("home-resume", "Home freezes VM; Resume continues same VM"),
        ("task-close", "confirmed task close needs no second decision"),
        ("tile-continue", "tile opens home; Continue resumes same VM"),
        ("tile-icon", "Continue shows the running MIDlet icon"),
        ("tile-resize", "Continue resynchronizes the guest surface after home resize"),
        ("tile-picker-cancel", "picker cancellation preserves VM; Continue resumes it"),
        ("restart-cancel", "one close confirmation; Cancel preserves current VM"),
        ("restart-confirm", "one close confirmation; ready for process restart"),
        ("restart-invalid", "invalid replacement preserves current VM"),
        ("restart-missing-class", "missing MIDlet class preserves current VM"),
        ("restart-missing-class-multi", "missing MIDlet class preserves current VM"),
    ],
)
def test_document_lifecycle_preserves_user_decisions_and_vm(
    phoneme_app: Path, tmp_path: Path, scenario: str, expected: str
) -> None:
    jar = tmp_path / 'Игра "кавычки".JAR'
    second = tmp_path / "replacement.jar"
    write_suite(jar, multiple=scenario.startswith("chooser"), icon=scenario == "tile-icon")
    if scenario == "restart-invalid":
        second.write_bytes(b"not a JAR")
    elif scenario.startswith("restart-missing-class"):
        write_suite(second, multiple=scenario.endswith("multi"), missing_class=True)
    else:
        write_suite(second)
    result = run_process(
        [str(phoneme_app)],
        name=f"actual phoneME shell {scenario}",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": scenario,
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_SECOND_JAR": str(second),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    lines = result.stdout.splitlines()
    assert lines[-1] == expected
    storage = [
        Path(line.removeprefix("storage ")) for line in lines if line.startswith("storage ")
    ]
    assert len(storage) == (0 if scenario == "chooser-cancel" else 1)
    assert all(path.is_dir() for path in storage)
    preferences = tmp_path / "preferences"
    if scenario in {"chooser-cancel", "start-failure", "early-exit", "early-error"}:
        assert not preferences.exists()
    else:
        assert "recent-documents=" in preferences.read_text(encoding="utf-8")


@pytest.mark.parametrize(
    ("scenario", "expected", "published"),
    [
        ("save-unpublished", "unpublished save reports failure", False),
        ("save-unsynced", "published save reports unconfirmed durability", True),
    ],
)
def test_recent_save_failure_reports_publication_state(
    phoneme_app: Path, tmp_path: Path, scenario: str, expected: str, *, published: bool
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    result = run_process(
        [str(phoneme_app)],
        name=f"phoneME preferences {scenario}",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": scenario,
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines()[-1] == expected
    assert (tmp_path / "preferences").exists() is published


@pytest.mark.parametrize("scenario", ["settings-save-unpublished", "settings-save-unsynced"])
def test_settings_stay_consistent_after_save_warning_and_recent_save(
    phoneme_app: Path, tmp_path: Path, scenario: str
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    result = run_process(
        [str(phoneme_app)],
        name=f"phoneME {scenario} followed by Recent save",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": scenario,
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines()[-1] == (
        "settings remain consistent after save warning and Recent save"
    )
    preferences = (tmp_path / "preferences").read_text(encoding="utf-8")
    assert 'view-settings={"scale":"integer","rotation":"90"}\n' in preferences
    assert "canvas-size=3\n" in preferences
    assert "recent-documents=[]\n" in preferences


def test_chooser_rms_failure_clears_busy_and_reports_error(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar, multiple=True)
    (tmp_path / "rms").write_text("not a directory", encoding="utf-8")
    result = run_process(
        [str(phoneme_app)],
        name="phoneME chooser RMS failure",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": "chooser-rms-failure",
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "RMS failure clears busy and reports error without starting VM\n"


@pytest.mark.parametrize(("kind", "error"), [("symlink", errno.ELOOP), ("fifo", errno.EINVAL)])
def test_document_rejects_nonregular_file_without_blocking(
    phoneme_app: Path, tmp_path: Path, kind: str, error: int
) -> None:
    jar = tmp_path / "input.jar"
    if kind == "symlink":
        actual = tmp_path / "actual.jar"
        write_suite(actual)
        jar.symlink_to(actual)
    else:
        os.mkfifo(jar)
    result = run_process(
        [str(phoneme_app)],
        name=f"phoneME document rejects {kind}",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": "invalid",
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == f"document error {error}; no VM started\n"

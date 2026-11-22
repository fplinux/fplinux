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
                "-Wl,--wrap=statfs",
                "-Wl,--wrap=fplinux_ui_state_dir",
                "-Wl,--wrap=fplinux_ui_prefs_open",
                "-lz",
                "-lmbedcrypto",
                "-ljson-c",
            ],
            name="compile actual phoneME document shell with controlled UI and VM",
        )
        yield executable


def write_suite(
    path: Path,
    *,
    multiple: bool = False,
    icon: bool = False,
    missing_class: bool = False,
    suite_name: str = "Example suite",
) -> None:
    """Create a normal ZIP with MIDP manifest fields and literal class entries."""
    icon_name = "icon.png" if icon else ""
    manifest = (
        f"MIDlet-Name: {suite_name}\r\nMIDlet-Vendor: Example maker\r\nMIDlet-Version: 1.0\r\n"
        f"MIDlet-1: First, {icon_name}, example.First\r\n"
    )
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
        ("vm-post-slice-delay", "VM timer starts its delay after the completed slice"),
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


def launched_heap_kib(
    phoneme_app: Path,
    jar: Path,
    root: Path,
    *,
    filesystem: str,
    scenario: str = "single",
    selection: str | None = None,
) -> int:
    env = os.environ | {
        "HOME": str(root),
        "LC_ALL": "C",
        "FPLINUX_PM_TEST_SCENARIO": scenario,
        "FPLINUX_PM_TEST_JAR": str(jar),
        "FPLINUX_PM_TEST_ROOT": str(root),
        "FPLINUX_PM_TEST_ROOT_FS": filesystem,
    }
    if selection is not None:
        env["FPLINUX_PM_TEST_HEAP_SELECTION"] = selection
    result = run_process(
        [str(phoneme_app)],
        name=f"phoneME Java memory ({scenario}, {selection or filesystem})",
        env=env,
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    sizes = [
        line.removeprefix("heap-kib=")
        for line in result.stdout.splitlines()
        if line.startswith("heap-kib=")
    ]
    assert len(sizes) == 1, result.stdout
    return int(sizes[0])


@pytest.mark.parametrize(
    ("filesystem", "heap_kib"),
    [("overlay", 4096), ("ext4", 8192), ("unavailable", 4096)],
)
def test_java_heap_default_follows_system_root(
    phoneme_app: Path, tmp_path: Path, filesystem: str, heap_kib: int
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)

    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem=filesystem) == heap_kib


@pytest.mark.parametrize(
    ("selection", "filesystem", "heap_kib"),
    [("4 MiB", "ext4", 4096), ("8 MiB", "overlay", 8192), ("16 MiB", "overlay", 16384)],
)
def test_global_java_memory_choice_persists_for_new_launches(
    phoneme_app: Path, tmp_path: Path, selection: str, filesystem: str, heap_kib: int
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)

    assert (
        launched_heap_kib(
            phoneme_app,
            jar,
            tmp_path,
            filesystem=filesystem,
            scenario="heap-settings-global",
            selection=selection,
        )
        == heap_kib
    )
    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem=filesystem) == heap_kib


def test_global_java_memory_use_default_restores_system_root_policy(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    (tmp_path / "preferences").write_text("heap=16\n", encoding="utf-8")

    assert (
        launched_heap_kib(
            phoneme_app,
            jar,
            tmp_path,
            filesystem="ext4",
            scenario="heap-settings-global",
            selection="Use default",
        )
        == 8192
    )
    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem="overlay") == 4096


def test_program_java_memory_selection_and_reset_take_effect_on_relaunch(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    other_jar = tmp_path / "other.jar"
    write_suite(jar)
    write_suite(other_jar, suite_name="Other suite")
    (tmp_path / "preferences").write_text("heap=8\n", encoding="utf-8")

    assert (
        launched_heap_kib(
            phoneme_app,
            jar,
            tmp_path,
            filesystem="overlay",
            scenario="heap-settings-program",
            selection="16 MiB",
        )
        == 8192
    )
    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem="overlay") == 16384
    assert launched_heap_kib(phoneme_app, other_jar, tmp_path, filesystem="overlay") == 8192

    assert (
        launched_heap_kib(
            phoneme_app,
            jar,
            tmp_path,
            filesystem="overlay",
            scenario="heap-settings-program",
            selection="Use default",
        )
        == 16384
    )
    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem="overlay") == 8192
    assert (
        launched_heap_kib(
            phoneme_app,
            jar,
            tmp_path,
            filesystem="ext4",
            scenario="heap-settings-global",
            selection="4 MiB",
        )
        == 4096
    )


@pytest.mark.parametrize("value", ["7", "large"])
def test_invalid_stored_global_java_memory_uses_system_root_default(
    phoneme_app: Path, tmp_path: Path, value: str
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    (tmp_path / "preferences").write_text(f"heap={value}\n", encoding="utf-8")

    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem="ext4") == 8192


def test_invalid_stored_program_java_memory_uses_global_choice(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    (tmp_path / "preferences").write_text("heap=8\n", encoding="utf-8")
    launched_heap_kib(
        phoneme_app,
        jar,
        tmp_path,
        filesystem="overlay",
        scenario="heap-settings-program",
        selection="16 MiB",
    )
    (program_preferences,) = tmp_path.glob("programs/*/preferences")
    program_preferences.write_text("heap=7\n", encoding="utf-8")

    assert launched_heap_kib(phoneme_app, jar, tmp_path, filesystem="overlay") == 8192


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


def launched_vm_mode(phoneme_app: Path, jar: Path, root: Path, scenario: str) -> str:
    result = run_process(
        [str(phoneme_app)],
        name=f"phoneME {scenario}",
        env=os.environ
        | {
            "HOME": str(root),
            "FPLINUX_PM_TEST_SCENARIO": scenario,
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(root),
        },
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    modes = [
        line.removeprefix("VM mode ")
        for line in result.stdout.splitlines()
        if line.startswith("VM mode ")
    ]
    assert len(modes) == 1, result.stdout
    return modes[0]


def test_global_jit_selection_applies_to_new_launches_after_restart(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)

    assert launched_vm_mode(phoneme_app, jar, tmp_path, "vm-settings-enable") == "JIT"
    assert launched_vm_mode(phoneme_app, jar, tmp_path, "vm-settings-reopen") == "JIT"


def test_program_vm_override_can_be_reset_from_recent(phoneme_app: Path, tmp_path: Path) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    (tmp_path / "preferences").write_text("jit=1\n", encoding="utf-8")

    assert launched_vm_mode(phoneme_app, jar, tmp_path, "vm-settings-override") == "JIT"
    assert launched_vm_mode(phoneme_app, jar, tmp_path, "vm-settings-reopen") == "Interpreter"
    assert launched_vm_mode(phoneme_app, jar, tmp_path, "vm-settings-recent-default") == "JIT"
    assert "jit=1" in (tmp_path / "preferences").read_text(encoding="utf-8").splitlines()


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


def write_descriptor(descriptor: Path, jar: Path, **overrides: object) -> None:
    attributes: dict[str, object] = {
        "MIDlet-Name": "Example suite",
        "MIDlet-Vendor": "Example maker",
        "MIDlet-Version": "1.0",
        "MIDlet-Jar-URL": jar.name,
        "MIDlet-Jar-Size": jar.stat().st_size,
        "MIDlet-1": "Named by JAD, , example.First",
    }
    attributes.update(overrides)
    descriptor.write_text(
        "".join(f"{name}: {value}\n" for name, value in attributes.items()), encoding="utf-8"
    )


def test_descriptor_opens_named_local_jar_and_is_saved_in_recent(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "a game.JAR"
    descriptor = tmp_path / "properties.JAD"
    write_suite(jar)
    write_descriptor(descriptor, jar, **{"MIDlet-Jar-URL": "a%20game.JAR"})

    result = run_process(
        [str(phoneme_app)],
        name="phoneME opens local descriptor",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": "jad-local",
            "FPLINUX_PM_TEST_JAR": str(descriptor),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines()[-2:] == [
        f"JAR {jar}",
        "first MIDlet screen accepts document; one VM started",
    ]
    assert "properties.JAD" in (tmp_path / "preferences").read_text(encoding="utf-8")


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("MIDlet-Name", "Other game"),
        ("MIDlet-Vendor", "Other maker"),
        ("MIDlet-Version", "1.1"),
        ("MIDlet-Jar-Size", "0"),
        ("MIDlet-Jar-URL", "https://example.com/game.jar"),
        ("MIDlet-Jar-URL", "%2e%2e%2fgame.jar"),
        ("MIDlet-Jar-URL", "game.jar?later"),
    ],
)
def test_descriptor_rejects_unmatched_identity_size_or_nonlocal_jar(
    phoneme_app: Path, tmp_path: Path, field: str, value: str
) -> None:
    jar = tmp_path / "game.jar"
    descriptor = tmp_path / "game.jad"
    write_suite(jar)
    write_descriptor(descriptor, jar, **{field: value})

    result = run_process(
        [str(phoneme_app)],
        name=f"phoneME rejects a mismatched descriptor {field}",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": "invalid",
            "FPLINUX_PM_TEST_JAR": str(descriptor),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == f"document error {errno.EBADMSG}; no VM started\n"


def test_opening_jar_ignores_a_companion_that_names_another_jar(
    phoneme_app: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    other = tmp_path / "other.jar"
    write_suite(jar)
    write_suite(other)
    write_descriptor(tmp_path / "game.jad", other)

    result = run_process(
        [str(phoneme_app)],
        name="phoneME retains selected JAR with mismatched companion",
        env=os.environ
        | {
            "FPLINUX_PM_TEST_SCENARIO": "unmatched-companion",
            "FPLINUX_PM_TEST_JAR": str(jar),
            "FPLINUX_PM_TEST_ROOT": str(tmp_path),
        },
        timeout=10,
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines()[-2:] == [
        "unmatched companion ignored",
        "first MIDlet screen accepts document; one VM started",
    ]

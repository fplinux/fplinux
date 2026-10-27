# SPDX-License-Identifier: GPL-2.0-only
"""Real phoneME document shell with controlled UI and VM boundaries, without Java execution."""

from __future__ import annotations

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
                str(PORT / "port/lf-screens/fplinux-pm-lf.c"),
                str(PORT / "port/lf-screens/fplinux-pm-lf-trace.c"),
                str(PORT / "vm/fplinux-pm-jar.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                *gettext_link_flags(),
                "-lz",
            ],
            name="compile actual phoneME document shell with controlled UI and VM",
        )
        yield executable


def write_suite(path: Path, *, multiple: bool = False) -> None:
    """Create a normal ZIP with MIDP manifest fields and literal class entries."""
    manifest = "MIDlet-Name: Example suite\r\nMIDlet-1: First, , example.First\r\n"
    if multiple:
        manifest += "MIDlet-2: Second, , example.Second\r\n"
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("META-INF/MANIFEST.MF", manifest)
        archive.writestr("example/First.class", b"\xca\xfe\xba\xbe")
        if multiple:
            archive.writestr("example/Second.class", b"\xca\xfe\xba\xbe")


@pytest.mark.parametrize("arguments", [["-h"], ["--help"], ["--unknown", "--help"]])
def test_help_precedes_ui_and_storage(phoneme_app: Path, arguments: list[str]) -> None:
    result = run_process(
        [str(phoneme_app), *arguments], name="phoneME help before resources", timeout=10
    )
    assert result.returncode == 0
    assert "Usage: fplinux-phoneme" in result.stdout
    assert "Record stores are temporary" in result.stdout
    assert result.stderr == ""


@pytest.mark.parametrize("argument", ["--unknown", "/games/example.jar"])
def test_cli_rejects_options_and_argv_document_routing(phoneme_app: Path, argument: str) -> None:
    result = run_process(
        [str(phoneme_app), argument], name="phoneME CLI error before UI", timeout=10
    )
    assert result.returncode == 2
    assert result.stdout == ""
    assert "fplinux-phoneme:" in result.stderr


def test_startup_waits_for_document_without_card_or_vm(phoneme_app: Path) -> None:
    result = run_process([str(phoneme_app)], name="phoneME empty startup", timeout=10)
    assert result.returncode == 0
    assert result.stdout == "waiting for Files; no VM started\n"
    assert result.stderr == ""


@pytest.mark.parametrize(
    ("scenario", "expected"),
    [
        ("single", "first MIDlet screen accepts document; one VM started"),
        ("chooser-start", "first MIDlet screen accepts document; one VM started"),
        ("chooser-cancel", "chooser cancelled; no VM started"),
        ("start-failure", "failed VM start reports error before acceptance"),
        ("replace-busy", "refused replace preserves peer and pending document; retry accepts"),
        ("replace-missing", "refused replace preserves peer and pending document; retry accepts"),
        ("push-busy", "refused push preserves peer and pending document; retry accepts"),
        ("push-missing", "refused push preserves peer and pending document; retry accepts"),
        ("home-resume", "Home freezes VM; Resume continues same VM"),
        ("restart-cancel", "one close confirmation; Cancel preserves current VM"),
        ("restart-confirm", "one close confirmation; ready for process restart"),
        ("restart-invalid", "invalid replacement preserves current VM"),
    ],
)
def test_document_lifecycle_preserves_user_decisions_and_vm(
    phoneme_app: Path, tmp_path: Path, scenario: str, expected: str
) -> None:
    jar = tmp_path / 'Игра "кавычки".JAR'
    second = tmp_path / "replacement.jar"
    write_suite(jar, multiple=scenario.startswith("chooser"))
    if scenario == "restart-invalid":
        second.write_bytes(b"not a JAR")
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
    assert all(not path.exists() for path in storage)


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
        env=os.environ | {"FPLINUX_PM_TEST_SCENARIO": "invalid", "FPLINUX_PM_TEST_JAR": str(jar)},
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == f"document error {error}; no VM started\n"

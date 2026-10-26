# SPDX-License-Identifier: GPL-2.0-only
"""Compile the literal tutorial and exercise help and native recipe installation.

This uses the real host UI libraries. It does not claim ARM compilation,
APK signing, an authenticated graphical session or physical-phone operation.
"""

from __future__ import annotations

import os
import shutil
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@pytest.fixture(scope="module")
def tutorial(tmp_path_factory: pytest.TempPathFactory, ui_host_inputs: Path) -> Iterator[Path]:
    """Use the complete named documents users copy, with current shared CLI sources."""
    guide = (ROOT / "site/src/content/docs/develop/app/first-app.mdx").read_text()
    with c_build_directory(tmp_path_factory, "first-app-tutorial") as directory:
        for language, filename in (
            ("c", "fplinux-example-counter.c"),
            ("ini", "org.example.counter.desktop"),
            ("sh", "APKBUILD"),
        ):
            opening = f'```{language} title="{filename}"\n'
            _, found, tail = guide.partition(opening)
            contents, closing, _ = tail.partition("\n```")
            if not found or not closing:
                raise ValueError(f"tutorial is missing its complete {filename} example")
            (directory / filename).write_text(contents + "\n")
        shutil.copyfile(ROOT / "lib/fplinux/fplinux-cli.c", directory / "fplinux-cli.c")
        shutil.copyfile(ROOT / "include/fplinux/fplinux-cli.h", directory / "fplinux-cli.h")
        run_process(
            [
                "sh",
                str(ROOT / "tests/host_tool/gui/fixtures/first-app/build.sh"),
                str(ROOT),
                str(directory),
                str(ui_host_inputs),
            ],
            name="compile and link the documented application with the real UI runtime",
            timeout=180,
            check=True,
        )
        yield directory


@pytest.mark.parametrize(
    ("arguments", "status"),
    [
        (["-h"], 0),
        (["--help"], 0),
        (["--unknown", "--help"], 0),
        (["--unknown"], 2),
        (["unexpected"], 2),
    ],
)
def test_tutorial_cli_finishes_before_opening_ui(
    tutorial: Path, tmp_path: Path, arguments: list[str], status: int
) -> None:
    state = tmp_path / "state"
    result = run_process(
        [str(tutorial / "fplinux-example-counter"), *arguments],
        name="documented application CLI without a session",
        timeout=10,
        env=os.environ
        | {
            "XDG_STATE_HOME": str(state),
            "FPLINUX_UI_SESSION_SOCKET": str(tmp_path / "missing.sock"),
            "WAYLAND_DISPLAY": "missing-tutorial-display",
        },
    )
    assert result.returncode == status, result.stdout + result.stderr
    if status == 0:
        assert "Usage: fplinux-example-counter" in result.stdout
        assert result.stderr == ""
    else:
        assert result.stdout == ""
        assert "fplinux-example-counter:" in result.stderr
    assert not state.exists()


def test_tutorial_recipe_checks_local_sources_and_installs_program(
    tutorial: Path, tmp_path: Path
) -> None:
    """Exercise native abuild source checks and package(), not an ARM APK build."""
    desktop = (tutorial / "org.example.counter.desktop").read_bytes()
    executable = (tutorial / "fplinux-example-counter").read_bytes()
    run_process(
        ["apkbuild-lint", "APKBUILD"],
        name="lint the tutorial recipe",
        timeout=30,
        cwd=tutorial,
        check=True,
    )
    run_process(
        ["abuild", "-F", "checksum", "verify"],
        name="generate and verify tutorial checksums from local sources",
        timeout=30,
        cwd=tutorial,
        env=os.environ | {"SRCDEST": str(tmp_path / "sources")},
        check=True,
    )
    installed = tmp_path / "package"
    run_process(
        ["sh", "-eu", "-c", '. "$1"; package', "tutorial-package", str(tutorial / "APKBUILD")],
        name="install the native tutorial binary using the documented package function",
        timeout=10,
        env=os.environ | {"srcdir": str(tutorial), "pkgdir": str(installed)},
        check=True,
    )
    program = installed / "usr/bin/fplinux-example-counter"
    entry = installed / "usr/share/applications/org.example.counter.desktop"
    assert program.read_bytes() == executable
    assert program.stat().st_mode & 0o777 == 0o755
    assert entry.read_bytes() == desktop
    assert entry.stat().st_mode & 0o777 == 0o644
    result = run_process(
        [str(program), "--help"], name="installed tutorial binary help", timeout=10, check=True
    )
    assert "Usage: fplinux-example-counter" in result.stdout

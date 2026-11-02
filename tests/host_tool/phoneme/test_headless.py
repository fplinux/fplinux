# SPDX-License-Identifier: GPL-2.0-only
"""Headless CLI and directory ownership with the VM replaced at its boundary."""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

PORT = ROOT / "alpine/aports/fplinux-phoneme"
HERE = ROOT / "tests/host_tool/phoneme"


@pytest.fixture(scope="module")
def headless(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "phoneme-headless") as directory:
        executable = directory / "headless"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{PORT / 'vm'}",
                str(HERE / "headless-boundary.c"),
                str(PORT / "vm/fplinux-pm-headless.c"),
                str(PORT / "vm/fplinux-pm-jar.c"),
                str(PORT / "port/canvas/fplinux-pm-canvas-core.c"),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                "-lz",
            ],
            name="compile phoneME headless CLI with a VM boundary",
        )
        yield executable


def test_help_does_not_open_requested_storage(headless: Path, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    result = run_process(
        [str(headless), "--storage-dir", str(storage), "--help"],
        name="headless help before storage",
        timeout=10,
    )
    assert result.returncode == 0
    assert "--storage-dir" in result.stdout
    assert not storage.exists()


def test_invalid_canvas_size_is_rejected_before_vm(headless: Path) -> None:
    result = run_process(
        [str(headless), "--canvas-size", "200x300", "suite.jar", "example.Main"],
        name="headless rejects unsupported Canvas size",
        timeout=10,
    )
    assert result.returncode == 2
    assert "invalid Canvas size" in result.stderr
    assert result.stdout == ""


def test_default_storage_is_removed_after_vm_exits(headless: Path) -> None:
    result = run_process(
        [str(headless), "suite.jar", "example.Main"], name="headless temporary store", timeout=10
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith("storage /tmp/fplinux-phoneme-headless.")
    assert not Path(result.stdout.removeprefix("storage ").strip()).exists()


def test_explicit_storage_is_preserved_and_passed_to_vm(headless: Path, tmp_path: Path) -> None:
    storage = tmp_path / "storage"
    storage.mkdir()
    result = run_process(
        [str(headless), "--storage-dir", str(storage), "suite.jar", "example.Main"],
        name="headless retained store",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == f"storage {storage}\n"
    assert (storage / "record").is_file()


def test_unusable_storage_reaches_vm_without_headless_replacement(headless: Path) -> None:
    result = run_process(
        [str(headless), "--storage-dir", "/dev/null/blocked", "suite.jar", "example.Main"],
        name="headless storage error boundary",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "storage /dev/null/blocked\n"

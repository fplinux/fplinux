# SPDX-License-Identifier: GPL-2.0-only
"""MRE broker file semantics on real host filesystems, with fsync errors only."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

APORT = ROOT / "alpine/aports/fplinux-mre"


@pytest.fixture(scope="module")
def vfs(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "mre-vfs") as directory:
        executable = directory / "vfs"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                str(ROOT / "tests/host_tool/mre/vfs.c"),
                str(APORT / "vfs-broker.c"),
                "-Wl,--wrap=fsync",
            ],
            name="compile production MRE broker and synchronization-error harness",
        )
        yield executable


@pytest.fixture
def roots(tmp_path: Path) -> tuple[Path, Path, Path]:
    executable = tmp_path / "documents"
    executable.mkdir()
    state = tmp_path / "state"
    private = state / "private"
    private.mkdir(parents=True)
    return executable, private, state


def run_case(vfs: Path, roots: tuple[Path, Path, Path], scenario: str) -> None:
    run_process(
        [str(vfs), scenario, *(str(path) for path in roots)],
        name=f"exercise MRE broker {scenario} on real files",
        timeout=10,
        check=True,
    )


def test_file_modes_bound_sizes_preserve_append_and_release_handles(
    vfs: Path, roots: tuple[Path, Path, Path]
) -> None:
    executable, private, _ = roots
    (executable / "large").write_bytes(b"old bytes beyond the file budget")
    run_case(vfs, roots, "modes")
    assert (private / "save").read_bytes() == b"Zbc+"
    assert (executable / "large").read_bytes() == b"\0" * 15 + b"!"
    assert not (private / "missing").exists()


def test_unicode_find_casefold_and_protected_names(
    vfs: Path, roots: tuple[Path, Path, Path]
) -> None:
    executable, private, _ = roots
    (executable / "Данные.TXT").write_bytes(b"companion")
    (executable / "game.VXP").write_bytes(b"executable")
    (executable / ".mre-vfs-hidden").write_bytes(b"private staging")
    (executable / "large").write_bytes(b"beyond the budget")
    (private / "save").write_bytes(b"private")
    (executable / "alias").symlink_to(private / "save")
    (executable / "link").symlink_to(private, target_is_directory=True)
    run_case(vfs, roots, "namespace")
    assert (private / "save").read_bytes() == b"private"
    assert (executable / "game.VXP").read_bytes() == b"executable"
    assert (executable / "Данные.TXT").read_bytes() == b"companion"


def test_copy_validates_then_copies_between_drives_without_replacement(
    vfs: Path, roots: tuple[Path, Path, Path]
) -> None:
    executable, private, _ = roots
    (executable / "source").write_bytes(b"copy me")
    run_case(vfs, roots, "copy")
    assert (executable / "source").read_bytes() == b"copy me"
    assert (private / "result").read_bytes() == b"copy me"
    assert sorted(path.name for path in private.iterdir()) == ["result"]


@pytest.mark.parametrize("operation", ["close", "copy"])
@pytest.mark.parametrize("synchronization", ["file", "directory"])
def test_sync_failure_reports_error_and_preserves_publication_state(
    vfs: Path,
    roots: tuple[Path, Path, Path],
    operation: str,
    synchronization: str,
) -> None:
    executable, private, _ = roots
    (executable / "source").write_bytes(b"copy me")
    run_case(vfs, roots, f"{operation}-{synchronization}")
    assert (executable / "source").read_bytes() == b"copy me"
    if operation == "close":
        assert (private / "save").read_bytes() == b"abc"
        assert sorted(path.name for path in private.iterdir()) == ["save"]
    elif synchronization == "directory":
        assert (private / "copied").read_bytes() == b"copy me"
        assert sorted(path.name for path in private.iterdir()) == ["copied"]
    else:
        assert list(private.iterdir()) == []

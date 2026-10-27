# SPDX-License-Identifier: GPL-2.0-only
"""MRE production path/state and broker objects on real host filesystems."""

from __future__ import annotations

import hashlib
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
def paths(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "mre-paths") as directory:
        executable = directory / "paths"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(APORT),
                str(ROOT / "tests/host_tool/mre/paths.c"),
                str(APORT / "runtime-paths.c"),
                str(APORT / "text-codec.c"),
                str(APORT / "vfs-broker.c"),
                "-lcrypto",
            ],
            name="compile MRE path and broker production objects",
        )
        yield executable


def test_normalized_path_identity_retains_state_on_update_and_changes_after_move(
    paths: Path, tmp_path: Path
) -> None:
    program = tmp_path / "game.vxp"
    program.write_bytes(b"sealed test bytes for path identity")
    state = tmp_path / "state"
    state.mkdir()
    normalized = str(program)
    key = hashlib.sha256(normalized.encode()).hexdigest()
    first = run_process(
        [str(paths), "prepare", str(tmp_path / "unused/../game.vxp"), str(state)],
        name="normalize MRE executable location and initialize C drive",
        timeout=10,
        check=True,
    )
    assert first.stdout.splitlines() == [normalized, key]
    saved = state / "apps" / key / "save"
    assert saved.read_bytes() == b"retained"
    program.write_bytes(b"updated VXP bytes at the same path")
    updated = run_process(
        [str(paths), "prepare", normalized, str(state)],
        name="retain MRE C drive after executable update",
        timeout=10,
        check=True,
    )
    assert updated.stdout == first.stdout
    assert saved.read_bytes() == b"retained"
    moved = tmp_path / "moved.vxp"
    program.rename(moved)
    relocated = run_process(
        [str(paths), "prepare", str(moved), str(state)],
        name="separate MRE C drive after executable move",
        timeout=10,
        check=True,
    )
    moved_key = hashlib.sha256(str(moved).encode()).hexdigest()
    assert relocated.stdout.splitlines() == [str(moved), moved_key]
    assert moved_key != key
    assert saved.read_bytes() == b"retained"
    assert (state / "apps" / moved_key / "save").read_bytes() == b"retained"


def test_executable_parent_remains_e_drive_but_c_root_and_ancestors_are_protected(
    paths: Path, tmp_path: Path
) -> None:
    state = tmp_path / "hidden/session/state"
    (state / "private").mkdir(parents=True)
    (state / "other-game").mkdir()
    (state / "other-game/save").write_bytes(b"private")
    (tmp_path / "companion.txt").write_bytes(b"companion")
    result = run_process(
        [str(paths), "namespace", str(tmp_path), str(state)],
        name="protect the whole MRE state root through the E drive",
        timeout=10,
        check=True,
    )
    assert result.stdout == "namespace: protected state and ancestors; companions and C: usable\n"
    assert (state / "other-game/save").read_bytes() == b"private"
    assert (state / "private/saves").is_dir()
    assert (tmp_path / "renamed").is_dir()


def test_symlink_executable_is_not_snapshotted(paths: Path, tmp_path: Path) -> None:
    (tmp_path / "original.vxp").write_bytes(b"regular executable bytes")
    link = tmp_path / "link.vxp"
    link.symlink_to("original.vxp")
    result = run_process(
        [str(paths), "prepare", str(link)],
        name="reject a symlink at the MRE executable boundary",
        timeout=10,
    )
    assert result.returncode == 1
    assert result.stdout.startswith("error:")

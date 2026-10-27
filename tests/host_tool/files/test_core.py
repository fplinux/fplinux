# SPDX-License-Identifier: GPL-2.0-only
"""Production Files objects, real temporary files, and controlled syscall errors."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

HERE = ROOT / "tests/host_tool/files"
APORT = ROOT / "alpine/aports/fplinux-files"
CONTENTS = b"Complete file\x00with binary bytes\xff\n" * 8192


@pytest.fixture(scope="module")
def files_core(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "files-core") as directory:
        executable = directory / "core"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                "-I",
                str(APORT),
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "include/fplinux"),
                str(HERE / "core.c"),
                str(APORT / "files-core.c"),
                str(APORT / "files-job.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                "-Wl,--wrap=fsync",
                "-Wl,--wrap=unlinkat",
                "-Wl,--wrap=write",
            ],
            name="compile Files core and worker production objects",
        )
        yield executable


@pytest.fixture
def directory(tmp_path: Path) -> Path:
    (tmp_path / "a-dir").mkdir()
    (tmp_path / "z-dir").mkdir()
    (tmp_path / "A.txt").write_bytes(b"A")
    (tmp_path / "source").write_bytes(CONTENTS)
    (tmp_path / "raw\udcff").write_bytes(b"raw")
    (tmp_path / "link").symlink_to("source")
    os.mkfifo(tmp_path / "fifo")
    return tmp_path


def invoke(executable: Path, scenario: str, *paths: Path) -> None:
    result = run_process(
        [str(executable), scenario, *(str(path) for path in paths)],
        name=f"Files production objects: {scenario}",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "files-core: passed\n"


@pytest.mark.parametrize("scenario", ["listing", "cancel_listing", "worker", "name", "search"])
def test_listing_worker_names_and_filter(files_core: Path, directory: Path, scenario: str) -> None:
    invoke(files_core, scenario, directory)


def test_directory_limit_discards_the_partial_result(files_core: Path, tmp_path: Path) -> None:
    for number in range(4097):
        (tmp_path / f"entry-{number}").touch()
    invoke(files_core, "overflow", tmp_path)


def test_stale_mounted_snapshot_rejects_underlying_directory_before_listing(
    files_core: Path, directory: Path
) -> None:
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fields = Path(f"/proc/self/fdinfo/{descriptor}").read_text().splitlines()
        mount_id = next(int(line.split()[1]) for line in fields if line.startswith("mnt_id:"))
    finally:
        os.close(descriptor)
    result = run_process(
        [str(files_core), "identity", str(directory), str(mount_id)],
        name="Files binds an opened directory to the mounted snapshot identity",
        timeout=10,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == "files-core: passed\n"


@pytest.mark.parametrize("scenario", ["copy", "sync", "cancel", "space", "conflict"])
def test_copy_no_replace_sync_retry_and_cancellation(
    files_core: Path, directory: Path, scenario: str
) -> None:
    destination = directory / "destination"
    if scenario == "conflict":
        destination.write_bytes(b"Existing destination")
    invoke(files_core, scenario, directory / "source", destination)
    assert (directory / "source").read_bytes() == CONTENTS
    if scenario in {"cancel", "space"}:
        assert not destination.exists()
    elif scenario == "conflict":
        assert destination.read_bytes() == b"Existing destination"
    else:
        assert destination.read_bytes() == CONTENTS
    assert not list(directory.glob(".fplinux-copy-*"))


@pytest.mark.parametrize("scenario", ["move", "rename"])
def test_same_filesystem_move_and_rename_remove_the_source(
    files_core: Path, directory: Path, scenario: str
) -> None:
    destination = directory / "destination"
    invoke(files_core, scenario, directory / "source", destination)
    assert not (directory / "source").exists()
    assert destination.read_bytes() == CONTENTS


@pytest.mark.parametrize("scenario", ["symlink", "symlink_parent", "fifo"])
def test_copy_does_not_follow_links_or_open_special_files(
    files_core: Path, directory: Path, scenario: str
) -> None:
    source = directory / ("fifo" if scenario == "fifo" else "link")
    if scenario == "symlink_parent":
        (directory / "parent-link").symlink_to(directory, target_is_directory=True)
        source = directory / "parent-link/source"
    destination = directory / "destination"
    invoke(files_core, scenario, source, destination)
    assert (directory / "source").read_bytes() == CONTENTS
    assert not destination.exists()


def test_directory_mutations_are_nonrecursive(files_core: Path, directory: Path) -> None:
    created = directory / "new"
    invoke(files_core, "mkdir", directory / "source", created)
    assert created.is_dir()
    renamed = directory / "renamed"
    invoke(files_core, "rename", created, renamed)
    assert not created.exists()
    assert renamed.is_dir()
    (renamed / "child").write_bytes(b"Keep")
    invoke(files_core, "nonempty", renamed, directory / "unused")
    assert (renamed / "child").read_bytes() == b"Keep"
    (renamed / "child").unlink()
    invoke(files_core, "remove", renamed, directory / "unused")
    assert not renamed.exists()


@pytest.mark.parametrize("scenario", ["mkdir_sync", "rename_sync", "remove_sync"])
def test_metadata_mutations_retry_sync_without_repeating_the_visible_change(
    files_core: Path, directory: Path, scenario: str
) -> None:
    source = directory / "source"
    destination = directory / "destination"
    invoke(files_core, scenario, source, destination)
    if scenario == "mkdir_sync":
        assert source.read_bytes() == CONTENTS
        assert destination.is_dir()
    elif scenario == "rename_sync":
        assert not source.exists()
        assert destination.read_bytes() == CONTENTS
    else:
        assert not source.exists()
        assert not destination.exists()


@pytest.fixture
def other_filesystem(directory: Path) -> Iterator[Path]:
    shared_memory = Path("/dev/shm")  # noqa: S108 -- EXDEV needs a real second filesystem.
    if not shared_memory.is_dir() or not os.access(shared_memory, os.W_OK):
        pytest.skip("a writable second filesystem is unavailable")
    if shared_memory.stat().st_dev == directory.stat().st_dev:
        pytest.skip("temporary files and shared memory use the same filesystem")
    with tempfile.TemporaryDirectory(prefix="fplinux-files-", dir=shared_memory) as path:
        yield Path(path)


@pytest.mark.parametrize("scenario", ["move", "move_dest_sync", "move_unlink", "move_source_sync"])
def test_cross_filesystem_move_preserves_reached_stages_and_retries_cleanup(
    files_core: Path, directory: Path, other_filesystem: Path, scenario: str
) -> None:
    destination = other_filesystem / "destination"
    invoke(files_core, scenario, directory / "source", destination)
    assert not (directory / "source").exists()
    assert destination.read_bytes() == CONTENTS
    assert not list(other_filesystem.glob(".fplinux-copy-*"))

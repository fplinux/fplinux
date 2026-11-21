# SPDX-License-Identifier: GPL-2.0-only
"""Suite identity and private RecordStore directory through the JAR reader."""

from __future__ import annotations

import errno
import zipfile
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
def store_tool(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "phoneme-store") as directory:
        executable = directory / "store-boundary"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include'}",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{PORT / 'port/app'}",
                f"-I{PORT / 'vm'}",
                str(HERE / "store-boundary.c"),
                str(PORT / "port/app/fplinux-pm-store.c"),
                str(PORT / "vm/fplinux-pm-jar.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
                "-lcrypto",
                "-lz",
            ],
            name="compile phoneME suite storage boundary",
        )
        yield executable


def write_suite(
    path: Path, *, name: str = "Game", vendor: str = "Maker", version: str = "1"
) -> None:
    manifest = (
        f"MIDlet-Name: {name}\r\n"
        f"MIDlet-Vendor: {vendor}\r\n"
        f"MIDlet-Version: {version}\r\n"
        "MIDlet-1: Game, , example.Game\r\n\r\n"
    )
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("META-INF/MANIFEST.MF", manifest)


def open_store(tool: Path, root: Path, jar: Path) -> Path:
    result = run_process([str(tool), str(root), str(jar)], name="open suite storage", timeout=10)
    assert result.returncode == 0, result.stdout + result.stderr
    prefs_key, path = result.stdout.splitlines()
    assert Path(path) == root / "rms" / prefs_key
    return Path(path)


def test_suite_update_and_move_reuse_store_but_other_identities_do_not(
    store_tool: Path, tmp_path: Path
) -> None:
    root = tmp_path / "state"
    root.mkdir(mode=0o700)
    jar = tmp_path / "first.jar"
    write_suite(jar)
    original = open_store(store_tool, root, jar)
    assert original.name == "e27cfd61a9bf6c0b5769b7d3faf2d35f639b070136294eeeebb3aaa69a07e2b6"
    (original / "record").write_bytes(b"saved\x00data")
    assert original.stat().st_mode & 0o777 == 0o700

    moved = tmp_path / "updated.jar"
    write_suite(moved, version="2")
    assert open_store(store_tool, root, moved) == original
    assert (original / "record").read_bytes() == b"saved\x00data"

    for name, vendor in [("game", "Maker"), ("Game", "maker"), ("Other", "Maker")]:
        write_suite(moved, name=name, vendor=vendor)
        assert open_store(store_tool, root, moved) != original

    write_suite(moved, name="c", vendor="ab")
    first = open_store(store_tool, root, moved)
    write_suite(moved, name="bc", vendor="a")
    assert open_store(store_tool, root, moved) != first
    write_suite(moved, name="x" * 512 + "1")
    first = open_store(store_tool, root, moved)
    write_suite(moved, name="x" * 512 + "2")
    assert open_store(store_tool, root, moved) != first


@pytest.mark.parametrize(("name", "vendor"), [("", "Maker"), ("Game", ""), ("", "")])
def test_incomplete_suite_identity_uses_normalized_absolute_jar_path(
    store_tool: Path, tmp_path: Path, name: str, vendor: str
) -> None:
    root = tmp_path / "state"
    root.mkdir(mode=0o700)
    first = tmp_path / "first.jar"
    second = tmp_path / "second.jar"
    write_suite(first, name=name, vendor=vendor)
    write_suite(second, name=name, vendor=vendor)
    path = open_store(store_tool, root, first)
    (tmp_path / "subdirectory").mkdir()
    assert open_store(store_tool, root, tmp_path / "subdirectory" / ".." / first.name) == path
    assert open_store(store_tool, root, second) != path


def test_storage_failure_is_reported_without_temporary_fallback(
    store_tool: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "game.jar"
    write_suite(jar)
    result = run_process(
        [str(store_tool), str(tmp_path / "missing"), str(jar)],
        name="missing suite storage root",
        timeout=10,
    )
    assert result.returncode == 1
    assert result.stdout == ""
    assert result.stderr == f"storage error {errno.ENOENT}\n"


def test_storage_rejects_a_nonprivate_state_directory(store_tool: Path, tmp_path: Path) -> None:
    root = tmp_path / "state"
    root.mkdir()
    root.chmod(0o755)
    jar = tmp_path / "game.jar"
    write_suite(jar)
    result = run_process(
        [str(store_tool), str(root), str(jar)], name="nonprivate state root", timeout=10
    )
    assert result.returncode == 1
    assert result.stderr == f"storage error {errno.EACCES}\n"
    assert not (root / "rms").exists()


def test_manifest_nul_is_rejected_instead_of_truncating_identity(
    store_tool: Path, tmp_path: Path
) -> None:
    jar = tmp_path / "bad.jar"
    with zipfile.ZipFile(jar, "w") as archive:
        archive.writestr(
            "META-INF/MANIFEST.MF",
            b"MIDlet-Name: Game\x00other\r\nMIDlet-Vendor: Maker\r\n"
            b"MIDlet-1: Game, , example.Game\r\n\r\n",
        )
    result = run_process(
        [str(store_tool), str(tmp_path), str(jar)], name="NUL in suite identity", timeout=10
    )
    assert result.returncode == 2
    assert result.stderr == "invalid suite\n"

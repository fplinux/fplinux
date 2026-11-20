# SPDX-License-Identifier: GPL-2.0-only
"""Read the startup pair and persistent data from the generated ext4 partition."""

from __future__ import annotations

import tarfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine.rootfs_state import ROOTFS_NAME, write_receipt
from fplinux_cli.build.storage.card import build_partition, write_apk_state
from fplinux_cli.common import sha256_file
from fplinux_cli.manifests.targets import load_target

from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

_TARGET = "nokia-ta1618"
_OTHER_TARGET = "inoi-240-modern-4g"
_RECIPE = "a" * 64
_OS_RELEASE = b"NAME=FPLinux\nBUILD_ID=abcdef134578\n"
_NOTE = b"persistent note with exact bytes\n"
_WORLD = b"fplinux-session\nfplinux-clocks\n"


@pytest.fixture
def packaged_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Build a real cpio with both release files and shared state."""
    source = tmp_path / "source"
    for directory in (
        "etc/apk/keys",
        "etc/fplinux",
        "home/fplinux",
        "lib/apk/db",
        "lib/apk/keys",
        "lib/firmware/ums9117",
    ):
        (source / directory).mkdir(parents=True, exist_ok=True)
    (source / "etc/os-release").write_bytes(_OS_RELEASE)
    (source / "etc/apk/arch").write_bytes(b"armv7\n")
    (source / "etc/apk/world").write_bytes(_WORLD)
    (source / "etc/apk/keys/system.rsa.pub").write_bytes(b"distribution key bytes\n")
    (source / "etc/apk/keys/backup.txt").write_bytes(b"unrelated backup bytes\n")
    (source / "lib/apk/db/installed").write_bytes(b"P:fplinux-session\nV:1.0.0-r0\n\n")
    (source / "lib/apk/keys/local.rsa.pub").write_bytes(b"local public key bytes\n")
    (source / "lib/apk/keys/local.rsa.pub").chmod(0o640)
    (source / "etc/fplinux/brightness.conf").write_bytes(b"levels=1,2,3\n")
    (source / "home/fplinux/note.txt").write_bytes(_NOTE)
    (source / "lib/firmware/ums9117/device.bin").write_bytes(b"device firmware\n")
    archive = tmp_path / ROOTFS_NAME
    run_process(
        [
            "sh",
            "-c",
            'find . -print0 | cpio --null -o -H newc >"$1"',
            "root-archive",
            str(archive),
        ],
        cwd=source,
        name="pack a root for the real ext4 writer",
        timeout=5,
        check=True,
    )
    write_receipt(tmp_path, _RECIPE)
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE", "b" * 64)
    return archive


def test_card_apk_state_copies_solver_files_and_public_keys(
    packaged_root: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "apk-state.tar"
    write_apk_state(packaged_root, archive)

    expected = {
        "etc/apk/arch": (0o644, b"armv7\n"),
        "etc/apk/keys/system.rsa.pub": (0o644, b"distribution key bytes\n"),
        "etc/apk/world": (0o644, _WORLD),
        "lib/apk/db/installed": (0o644, b"P:fplinux-session\nV:1.0.0-r0\n\n"),
        "lib/apk/keys/local.rsa.pub": (0o640, b"local public key bytes\n"),
    }
    assert archive.stat().st_mode & 0o777 == 0o644
    with tarfile.open(archive) as payload:
        assert payload.getnames() == list(expected)
        for member in payload:
            assert member.isfile()
            assert (member.uid, member.gid) == (0, 0)
            mode, contents = expected[member.name]
            assert member.mode == mode
            source = payload.extractfile(member)
            assert source is not None
            with source:
                assert source.read() == contents

    repeated = tmp_path / "repeated.tar"
    write_apk_state(packaged_root, repeated)
    assert repeated.read_bytes() == archive.read_bytes()


def _debugfs(image: Path, command: str) -> str:
    return run_process(
        ["debugfs", "-R", command, str(image)],
        name="inspect the actual ext4 partition",
        timeout=5,
        check=True,
    ).stdout


def test_factory_pair_and_shared_tree_are_on_ext4(packaged_root: Path, tmp_path: Path) -> None:
    spec = load_target(_TARGET, "microsd-uboot")["image"]
    output = tmp_path / "factory"
    fit = tmp_path / "FPLINUX.ITB"
    first_fit = b"first independently verified FIT"
    fit.write_bytes(first_fit)
    image = build_partition(_TARGET, spec, packaged_root, _RECIPE, output, fit=fit)
    assert image.stat().st_size == 64 * 1024 * 1024
    assert _debugfs(image, "cat /system/active") == "a\n"
    assert _debugfs(image, "cat /system/target") == _TARGET + "\n"
    assert _debugfs(image, "cat /system/a/FPLINUX.ITB").encode() == first_fit
    assert _debugfs(image, "cat /system/a/root/etc/os-release").encode() == _OS_RELEASE
    assert _debugfs(image, "cat /system/a/base-world").encode() == _WORLD
    assert _debugfs(image, "cat /system/a/root/etc/fplinux/brightness.conf") == "levels=1,2,3\n"
    assert not _debugfs(image, "cat /system/a/root/home/fplinux/note.txt")
    assert _debugfs(image, "cat /data/home/fplinux/note.txt").encode() == _NOTE
    assert "Mode:  0700" in _debugfs(image, "stat /data/home/fplinux")
    assert _debugfs(image, "cat /data/lib/firmware/ums9117/device.bin") == "device firmware\n"
    assert not _debugfs(image, "stat /system/b")
    assert not _debugfs(image, "stat /system/a/ready")
    inode = image.stat().st_ino
    assert (
        build_partition(_TARGET, spec, packaged_root, _RECIPE, output, fit=fit).stat().st_ino
        == inode
    )
    before = sha256_file(image)
    new_fit = b"another independently verified FIT"
    fit.write_bytes(new_fit)
    build_partition(_TARGET, spec, packaged_root, _RECIPE, output, fit=fit)
    assert sha256_file(image) != before
    assert _debugfs(image, "cat /system/a/FPLINUX.ITB").encode() == new_fit


def test_install_partition_awaits_personalization_and_reuses_the_shared_root(
    packaged_root: Path, tmp_path: Path
) -> None:
    spec = load_target(_TARGET, "microsd-uboot")["image"]
    output = tmp_path / "install"
    image = build_partition(_TARGET, spec, packaged_root, _RECIPE, output, fit=None)
    assert _debugfs(image, "cat /system/a/root/etc/os-release").encode() == _OS_RELEASE
    for path in (
        "/system/active",
        "/system/target",
        "/system/a/FPLINUX.ITB",
        "/system/a/ready",
        "/system/a/base-world",
    ):
        assert not _debugfs(image, f"stat {path}")
    assert "Mode:  0700" in _debugfs(image, "stat /data/etc/fplinux/card-access")
    inode = image.stat().st_ino
    reused = build_partition(_OTHER_TARGET, spec, packaged_root, _RECIPE, output, fit=None)
    assert reused.stat().st_ino == inode

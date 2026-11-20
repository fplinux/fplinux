# SPDX-License-Identifier: GPL-2.0-only
"""Read the startup pair and persistent data from the generated ext4 partition."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine.rootfs_state import ROOTFS_NAME, write_receipt
from fplinux_cli.build.storage.card import build_partition
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
    for directory in ("etc/apk", "etc/fplinux", "home/fplinux", "lib/firmware/ums9117"):
        (source / directory).mkdir(parents=True, exist_ok=True)
    (source / "etc/os-release").write_bytes(_OS_RELEASE)
    (source / "etc/apk/world").write_bytes(_WORLD)
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

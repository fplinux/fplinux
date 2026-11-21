# SPDX-License-Identifier: GPL-2.0-only
"""Prepare reproducible Alpine root filesystem and RAM bootstrap files."""

from __future__ import annotations

import os
import shlex
import shutil
import stat
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.card_kit import FIT_AUDIO_CPIO_BYTES
from fplinux_cli.build import process as process_build
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.build.inputs import require_file
from fplinux_cli.common import ROOT, fail, sha256_file

from .rootfs_state import INITRAMFS_NAME

if TYPE_CHECKING:
    from fplinux_cli.artifacts.release_identity import ReleaseContext

_INITRAMFS_RUNTIME = (
    "bin/busybox",
    "lib/ld-musl-armhf.so.1",
    "lib/libc.musl-armv7.so.1",
    "usr/lib/libgcc_s.so.1",
)


def _stamp_release(root: Path, release: ReleaseContext) -> None:
    """Stamp the composed image after package installation, leaving APKs reusable."""
    path = require_file(root / "etc/os-release")
    lines = [
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if not line.startswith(("VERSION_ID=", "BUILD_ID="))
    ]
    lines.extend((f"VERSION_ID={release.release_version}", f"BUILD_ID={release.release_id}"))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _brightness_config_text(display_brightness: dict[str, Any]) -> str:
    """Render the target's validated brightness table for the phone runtime."""
    levels = ",".join(str(level) for level in display_brightness["levels"])
    return f"backlight={display_brightness['backlight']}\nlevels={levels}\n"


def _install_display_brightness(root: Path, display_brightness: dict[str, Any] | None) -> None:
    if display_brightness is None:
        return
    directory = root / "etc/fplinux"
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / "brightness.conf"
    destination.write_text(_brightness_config_text(display_brightness), encoding="utf-8")
    destination.chmod(0o644)


def _install_default_soundfont(root: Path, *, root_kind: str) -> None:
    directory = root / "usr/share/fplinux/soundfonts"
    bank = "timgm6mb.sf2" if root_kind == "external" else "fm1gm.sf2"
    if (directory / bank).is_file():
        (directory / "default.sf2").symlink_to(bank)


def _normalize_rootfs(root: Path) -> None:
    timestamp = int(SOURCE_DATE_EPOCH)
    for path in [*sorted(root.rglob("*"), reverse=True), root]:
        os.utime(path, (timestamp, timestamp), follow_symlinks=False)


def _write_rootfs_cpio(root: Path, destination: Path) -> None:
    command = (
        "find . -xdev -print0 | LC_ALL=C sort -z | "
        "cpio --null --quiet --create --format=newc --reproducible --owner=0:0 "
        f"> {shlex.quote(str(destination))}"
    )
    process_build.run(["/bin/sh", "-c", command], cwd=root)
    require_file(destination)


def _write_squashfs(root: Path, output: Path) -> None:
    process_build.run(
        [
            "mksquashfs",
            str(root),
            str(output),
            "-noappend",
            "-no-progress",
            "-exit-on-error",
            "-processors",
            "1",
            "-all-root",
            "-exports",
            "-xattrs",
            "-xattrs-exclude",
            "^security[.]selinux$",
            "-b",
            "64K",
            "-comp",
            "xz",
            "-Xdict-size",
            "64K",
        ]
    )


def _hardlink_groups(entries: dict[Path, os.stat_result]) -> dict[Path, tuple[Path, ...]]:
    """Identify links by their paths within this composition."""
    by_inode: dict[tuple[int, int], list[Path]] = defaultdict(list)
    for path, metadata in entries.items():
        if metadata.st_nlink > 1 and not stat.S_ISDIR(metadata.st_mode):
            by_inode[metadata.st_dev, metadata.st_ino].append(path)
    groups = (tuple(sorted(paths)) for paths in by_inode.values() if len(paths) > 1)
    return {path: group for group in groups for path in group}


def _xattrs(path: Path) -> dict[str, bytes]:
    return {
        name: os.getxattr(path, name, follow_symlinks=False)
        for name in os.listxattr(path, follow_symlinks=False)
        if name != "security.selinux"
    }


def _same_lower_entry(
    target: Path, base: Path, target_stat: os.stat_result, base_stat: os.stat_result
) -> bool:
    # Both SquashFS layers use the same normalized timestamps and root ownership.
    if target_stat.st_mode != base_stat.st_mode or _xattrs(target) != _xattrs(base):
        return False
    if stat.S_ISREG(target_stat.st_mode):
        return target_stat.st_size == base_stat.st_size and sha256_file(target) == sha256_file(
            base
        )
    if stat.S_ISLNK(target_stat.st_mode):
        return target.readlink() == base.readlink()
    return target_stat.st_rdev == base_stat.st_rdev


def _stage_board_layer(root: Path, base: Path, layer: Path) -> None:
    """Stage the files the board composition adds or replaces in the shared root."""
    target_entries = {path.relative_to(root): path.lstat() for path in root.rglob("*")}
    base_entries = {path.relative_to(base): path.lstat() for path in base.rglob("*")}
    removed = sorted(base_entries.keys() - target_entries.keys())
    if removed:
        fail(f"board composition removes shared root path: /{removed[0]}")
    target_groups = _hardlink_groups(target_entries)
    base_groups = _hardlink_groups(base_entries)
    changed: set[Path] = set()
    directories = [Path()]
    layer.mkdir()
    for path, metadata in sorted(target_entries.items()):
        if stat.S_ISDIR(metadata.st_mode):
            (layer / path).mkdir()
            directories.append(path)
            continue
        original = base_entries.get(path)
        links = target_groups.get(path, (path,))
        if (
            original is None
            or links != base_groups.get(path, (path,))
            or not _same_lower_entry(root / path, base / path, metadata, original)
        ):
            changed.update(links)
    for path in sorted(changed):
        os.link(root / path, layer / path, follow_symlinks=False)
    for path in reversed(directories):
        shutil.copystat(root / path, layer / path)


def extract_rootfs(archive: Path, root: Path) -> None:
    """Extract a composed root archive with its normalized package modes and links."""
    root.mkdir()
    process_build.run(
        [
            "cpio",
            "--extract",
            "--quiet",
            "--make-directories",
            "--no-preserve-owner",
            "--preserve-modification-time",
            "--file",
            str(archive),
            "--directory",
            str(root),
        ]
    )


def write_card_rootfs_layer(rootfs: Path, base_rootfs: Path, destination: Path) -> None:
    """Pack the board additions and replacements for a shared writable card root."""
    with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".card-rootfs.") as name:
        temporary = Path(name)
        target = temporary / "target"
        base = temporary / "base"
        board = temporary / "board"
        extract_rootfs(rootfs, target)
        extract_rootfs(base_rootfs, base)
        _stage_board_layer(target, base, board)
        process_build.run(
            [
                "tar",
                "--create",
                "--format=ustar",
                "--sort=name",
                "--numeric-owner",
                "--owner=0",
                "--group=0",
                f"--mtime=@{SOURCE_DATE_EPOCH}",
                "--file",
                str(destination),
                "--directory",
                str(board),
                ".",
            ]
        )


def _write_board_squashfs(
    root: Path, staging: Path, bootstrap: Path, base_initramfs: Path
) -> None:
    process_build.run(
        [
            "cpio",
            "--extract",
            "--quiet",
            "--no-preserve-owner",
            "--file",
            str(base_initramfs),
            "--directory",
            str(bootstrap),
            "root.squashfs",
        ]
    )
    base = staging / "base"
    process_build.run(
        [
            "unsquashfs",
            "-no-progress",
            "-processors",
            "1",
            "-d",
            str(base),
            str(require_file(bootstrap / "root.squashfs")),
        ]
    )
    board = staging / "board"
    _stage_board_layer(root, base, board)
    _write_squashfs(board, bootstrap / "board.squashfs")
    shutil.rmtree(board)
    shutil.rmtree(base)


def _write_ramroot_initramfs(
    root: Path, staging: Path, *, base_initramfs: Path | None = None
) -> None:
    """Pack the verified composition behind a small boot-only RAM filesystem."""
    bootstrap = staging / "bootstrap"
    bootstrap.mkdir()
    for directory in ("bin", "lib", "usr", "usr/lib", "dev", "proc", "sys", "run", "newroot"):
        (bootstrap / directory).mkdir(mode=0o755)
    # BusyBox and its shared libraries come from the same locked composed runtime.
    for relative in _INITRAMFS_RUNTIME:
        source = root / relative
        destination = bootstrap / relative
        if source.is_symlink():
            destination.symlink_to(source.readlink())
        else:
            shutil.copy2(require_file(source), destination)
    (bootstrap / "bin/sh").symlink_to("busybox")
    shutil.copyfile(require_file(ROOT / "alpine/ramroot-init.sh"), bootstrap / "init")
    (bootstrap / "init").chmod(0o755)
    if base_initramfs is None:
        _write_squashfs(root, bootstrap / "root.squashfs")
    else:
        _write_board_squashfs(root, staging, bootstrap, base_initramfs)
    _normalize_rootfs(bootstrap)
    _write_rootfs_cpio(bootstrap, staging / INITRAMFS_NAME)
    shutil.rmtree(bootstrap)


def write_card_initramfs(  # noqa: PLR0913 -- the composed root and FIT inputs stay explicit.
    rootfs: Path,
    output: Path,
    *,
    target: str,
    config: dict[str, Any],
    build_id: str,
    audio: bytes,
) -> None:
    """Pack the early userspace and the replaceable audio archive for the card FIT."""
    if len(audio) > FIT_AUDIO_CPIO_BYTES:
        fail("prepared audio firmware exceeds the FIT ramdisk slot")
    storage = config["storage"]
    identity = {
        "target": f"{target}\n".encode("ascii"),
        "build-id": f"{build_id}\n".encode("ascii"),
        "disk-id": storage["disk_signature"].to_bytes(4, "little"),
        "root-start": f"{storage['root_offset'] // 512}\n".encode("ascii"),
        "root-uuid": f"{storage['root_uuid']}\n".encode("ascii"),
        "wait-seconds": f"{config['linux']['root']['wait_seconds']}\n".encode("ascii"),
    }
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".card-initramfs.") as name:
        staging = Path(name)
        bootstrap = staging / "root"
        bootstrap.mkdir()
        for directory in (
            "dev",
            "proc",
            "sys",
            "run",
            "newroot",
            "etc/fplinux",
            "etc/fplinux-card",
        ):
            (bootstrap / directory).mkdir(parents=True)
        process_build.run(
            [
                "cpio",
                "--extract",
                "--quiet",
                "--make-directories",
                "--no-preserve-owner",
                "--file",
                str(rootfs),
                *_INITRAMFS_RUNTIME,
            ],
            cwd=bootstrap,
        )
        for relative in _INITRAMFS_RUNTIME:
            member = bootstrap / relative
            if not member.is_file() or not member.resolve().is_relative_to(bootstrap):
                fail(f"missing initramfs runtime file: {relative}")
        (bootstrap / "bin/sh").symlink_to("busybox")
        shutil.copyfile(require_file(ROOT / "alpine/cardroot-init.sh"), bootstrap / "init")
        (bootstrap / "init").chmod(0o755)
        directories = bootstrap / "etc/fplinux/card-data-directories"
        shutil.copyfile(require_file(ROOT / "alpine/card-data-directories"), directories)
        directories.chmod(0o644)
        for filename, contents in identity.items():
            (bootstrap / "etc/fplinux-card" / filename).write_bytes(contents)
        _normalize_rootfs(bootstrap)
        ramdisk = staging / output.name
        _write_rootfs_cpio(bootstrap, ramdisk)
        with ramdisk.open("ab") as stream:
            stream.write(audio.ljust(FIT_AUDIO_CPIO_BYTES, b"\0"))
        ramdisk.chmod(0o600)
        ramdisk.replace(output)

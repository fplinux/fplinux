# SPDX-License-Identifier: GPL-2.0-only
"""Prepare reproducible Alpine root filesystem and RAM bootstrap files."""

from __future__ import annotations

import os
import shlex
import shutil
from typing import TYPE_CHECKING, Any

from fplinux_cli.build import process as process_build
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.build.inputs import require_file
from fplinux_cli.common import ROOT

from .rootfs_state import INITRAMFS_NAME

if TYPE_CHECKING:
    from pathlib import Path


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


def _write_ramroot_initramfs(root: Path, staging: Path) -> None:
    """Pack the verified composition behind a small boot-only RAM filesystem."""
    bootstrap = staging / "bootstrap"
    bootstrap.mkdir()
    for directory in ("bin", "lib", "usr", "usr/lib", "dev", "proc", "sys", "run", "newroot"):
        (bootstrap / directory).mkdir(mode=0o755)
    # BusyBox and its shared libraries come from the same locked composed runtime.
    for relative in (
        "bin/busybox",
        "lib/ld-musl-armhf.so.1",
        "lib/libc.musl-armv7.so.1",
        "usr/lib/libgcc_s.so.1",
    ):
        source = root / relative
        destination = bootstrap / relative
        if source.is_symlink():
            destination.symlink_to(source.readlink())
        else:
            shutil.copy2(require_file(source), destination)
    (bootstrap / "bin/sh").symlink_to("busybox")
    shutil.copyfile(require_file(ROOT / "alpine/ramroot-init.sh"), bootstrap / "init")
    (bootstrap / "init").chmod(0o755)
    process_build.run(
        [
            "mksquashfs",
            str(root),
            str(bootstrap / "root.squashfs"),
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
    _normalize_rootfs(bootstrap)
    _write_rootfs_cpio(bootstrap, staging / INITRAMFS_NAME)
    shutil.rmtree(bootstrap)

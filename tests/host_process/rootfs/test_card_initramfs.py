# SPDX-License-Identifier: GPL-2.0-only
"""Check the FIT handoff archive against a composed-root input."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.alpine import rootfs_files
from fplinux_cli.artifacts.newc import newc_entries
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.manifests.targets import load_target

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


def test_card_initramfs_contains_only_the_fit_bootstrap_and_audio_slot(tmp_path: Path) -> None:
    root = tmp_path / "root"
    libraries = {
        "bin/busybox": b"packaged busybox",
        "lib/ld-musl-armhf.so.1": b"packaged libc",
        "usr/lib/libgcc_s.so.1": b"packaged libgcc",
    }
    for relative, data in {**libraries, "etc/os-release": b"BUILD_ID=0123456789ab\n"}.items():
        path = root / relative
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_bytes(data)
    (root / "bin/busybox").chmod(0o755)
    (root / "lib/libc.musl-armv7.so.1").symlink_to("ld-musl-armhf.so.1")
    composition = tmp_path / "rootfs.cpio"
    with composition.open("wb") as archive:
        subprocess.run(
            ["cpio", "--create", "--format=newc", "--quiet"],
            cwd=root,
            input=b"bin/busybox\nlib/ld-musl-armhf.so.1\nlib/libc.musl-armv7.so.1\n"
            b"usr/lib/libgcc_s.so.1\netc/os-release\n",
            stdout=archive,
            check=True,
            timeout=5,
        )
    target = "inoi-240-modern-4g"
    config = load_target(target, "microsd-uboot")
    audio = create_device_data_initramfs({})
    output = tmp_path / "fit-initramfs.cpio"

    rootfs_files.write_card_initramfs(
        composition, output, target=target, config=config, build_id="0123456789ab", audio=audio
    )

    ramdisk = output.read_bytes()
    base, slot = ramdisk[:-4096], ramdisk[-4096:]
    entries = {entry.name: entry for entry in newc_entries(base) if entry.data}
    assert {name: entries[name].data for name in libraries} == libraries
    assert (entries["bin/busybox"].mode, entries["init"].mode) == (0o100755, 0o100755)
    assert entries["init"].data == (ROOT / "alpine/cardroot-init.sh").read_bytes()
    assert all((entry.uid, entry.gid) == (0, 0) for entry in entries.values())
    assert entries["bin/sh"].data == b"busybox"
    assert entries["lib/libc.musl-armv7.so.1"].data == b"ld-musl-armhf.so.1"
    directories = entries["etc/fplinux/card-data-directories"]
    assert (directories.mode, directories.data) == (
        0o100644,
        (ROOT / "alpine/card-data-directories").read_bytes(),
    )
    assert {
        name.removeprefix("etc/fplinux-card/"): entry.data
        for name, entry in entries.items()
        if name.startswith("etc/fplinux-card/")
    } == {
        "target": b"inoi-240-modern-4g\n",
        "build-id": b"0123456789ab\n",
        "disk-id": b"XLPF",
        "root-start": b"133120\n",
        "root-uuid": b"042681b5-d000-5b78-9c16-8e8b2944594e\n",
        "wait-seconds": b"10\n",
    }
    assert set(entries) == {
        *libraries,
        "bin/sh",
        "lib/libc.musl-armv7.so.1",
        "init",
        "etc/fplinux/card-data-directories",
        "etc/fplinux-card/target",
        "etc/fplinux-card/build-id",
        "etc/fplinux-card/disk-id",
        "etc/fplinux-card/root-start",
        "etc/fplinux-card/root-uuid",
        "etc/fplinux-card/wait-seconds",
    }
    assert slot == audio.ljust(4096, b"\0")

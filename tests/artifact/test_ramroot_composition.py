# SPDX-License-Identifier: GPL-2.0-only
"""Inspect composed RAM artifacts with the declared host archive tools."""

from __future__ import annotations

import os
import stat
import struct
import subprocess
from pathlib import Path
from unittest import mock

from fplinux_cli.alpine import rootfs_files as alpine_builder

from tests.process import run_process


class RamrootCompositionTests:
    """Packing preserves lower metadata without embedding an unpacked second root."""

    def test_boot_archive_keeps_runtime_bytes_and_lower_content_metadata(
        self, tmp_path: Path
    ) -> None:
        """Real archive readers verify runtime metadata and exclude inherited host labels."""
        directory = tmp_path
        root = directory / "root"
        source = directory / "source"
        staging = directory / "output"
        for relative in ("bin", "lib", "usr/lib", "etc"):
            (root / relative).mkdir(parents=True)
        (source / "alpine").mkdir(parents=True)
        staging.mkdir()
        (source / "alpine/ramroot-init.sh").write_bytes(b"#!/bin/sh\nexit 0\n")
        (root / "bin/busybox").write_bytes(b"controlled BusyBox runtime bytes\n")
        (root / "bin/busybox").chmod(0o755)
        (root / "lib/ld-musl-armhf.so.1").write_bytes(b"controlled musl runtime bytes\n")
        (root / "lib/ld-musl-armhf.so.1").chmod(0o755)
        (root / "lib/libc.musl-armv7.so.1").symlink_to("ld-musl-armhf.so.1")
        (root / "usr/lib/libgcc_s.so.1").write_bytes(b"controlled libgcc runtime bytes\n")
        (root / "usr/lib/libgcc_s.so.1").chmod(0o644)
        private = root / "etc/private.conf"
        private.write_bytes(b"independent metadata fixture\n")
        private.chmod(0o600)
        os.setxattr(private, "user.fplinux.fixture", b"preserved")
        host_label_present = "security.selinux" in os.listxattr(private)
        os.link(private, root / "etc/private.link")
        (root / "etc/private.symlink").symlink_to("private.conf")

        with mock.patch.object(alpine_builder, "ROOT", source):
            alpine_builder._write_rootfs_cpio(root, staging / "rootfs.cpio")  # noqa: SLF001
            alpine_builder._write_ramroot_initramfs(root, staging)  # noqa: SLF001

        boot = directory / "boot"
        boot.mkdir()
        with (staging / "initramfs.cpio").open("rb") as archive:
            subprocess.run(
                [
                    "cpio",
                    "--quiet",
                    "--extract",
                    "--make-directories",
                    "--no-absolute-filenames",
                ],
                stdin=archive,
                cwd=boot,
                capture_output=True,
                check=True,
                timeout=10,
            )
        assert ((boot / "init").read_bytes()) == (b"#!/bin/sh\nexit 0\n")
        assert (stat.S_IMODE((boot / "init").stat().st_mode)) == (0o755)
        assert ((boot / "bin/busybox").read_bytes()) == (b"controlled BusyBox runtime bytes\n")
        assert ((boot / "lib/ld-musl-armhf.so.1").read_bytes()) == (
            b"controlled musl runtime bytes\n"
        )
        assert ((boot / "usr/lib/libgcc_s.so.1").read_bytes()) == (
            b"controlled libgcc runtime bytes\n"
        )
        assert (stat.S_IMODE((boot / "usr/lib/libgcc_s.so.1").stat().st_mode)) == (0o644)
        assert ((boot / "bin/sh").readlink()) == (Path("busybox"))
        assert ((boot / "lib/libc.musl-armv7.so.1").readlink()) == (Path("ld-musl-armhf.so.1"))
        assert not ((boot / "etc/private.conf").exists())
        lower_header = (boot / "root.squashfs").read_bytes()[:96]
        assert (lower_header[:4]) == (b"hsqs")
        assert (struct.unpack_from("<I", lower_header, 12)[0]) == (65536)
        if host_label_present:
            metadata = run_process(
                [
                    "unsquashfs",
                    "-xattrs-include",
                    "^security[.]selinux$",
                    "-pf",
                    "-",
                    str(boot / "root.squashfs"),
                ],
                name="RAM lower host-label inspection",
                timeout=10,
                check=True,
            )
            assert (" x security.selinux=") not in (metadata.stdout)
        assert ((staging / "rootfs.cpio").read_bytes()) != (
            (staging / "initramfs.cpio").read_bytes()
        )
        lower = directory / "lower"
        run_process(
            [
                "unsquashfs",
                "-no-progress",
                "-processors",
                "1",
                "-xattrs-include",
                "^user\\.",
                "-d",
                str(lower),
                str(boot / "root.squashfs"),
            ],
            name="RAM lower extraction",
            timeout=10,
            check=True,
        )
        extracted = lower / "etc/private.conf"
        assert (extracted.read_bytes()) == (b"independent metadata fixture\n")
        assert (stat.S_IMODE(extracted.stat().st_mode)) == (0o600)
        assert (os.getxattr(extracted, "user.fplinux.fixture")) == (b"preserved")
        assert (extracted.stat().st_ino) == ((lower / "etc/private.link").stat().st_ino)
        assert ((lower / "etc/private.symlink").readlink()) == (Path("private.conf"))

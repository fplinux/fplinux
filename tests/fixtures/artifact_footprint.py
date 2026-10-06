# SPDX-License-Identifier: GPL-2.0-only
"""Independent archive, ELF and SquashFS inputs for artifact measurements."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import lzma
import struct
import tarfile
from typing import TYPE_CHECKING

from fplinux_cli.artifacts.bundles import CurrentBundle

if TYPE_CHECKING:
    from pathlib import Path


def newc(entries: list[tuple[str, bytes, int, int, int]]) -> bytes:
    """Write test-owned newc records: name, payload, mode, inode, link count."""
    archive = bytearray()
    for name, payload, mode, inode, links in [*entries, ("TRAILER!!!", b"", 0, 0, 1)]:
        encoded_name = name.encode() + b"\0"
        fields = (inode, mode, 0, 0, links, 0, len(payload), 0, 0, 0, 0, len(encoded_name), 0)
        archive.extend(b"070701" + "".join(f"{value:08x}" for value in fields).encode())
        archive.extend(encoded_name)
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(payload)
        archive.extend(b"\0" * (-len(archive) % 4))
    return bytes(archive)


def kernel_with_initramfs(cpio: bytes, compression: str) -> bytes:
    """Provide one ARM ELF32 section containing size then compressed payload."""
    compressed = gzip.compress(cpio, mtime=0) if compression == "gzip" else lzma.compress(cpio)
    section = struct.pack("<I", len(compressed)) + compressed
    header = bytearray(52)
    header[:16] = b"\x7fELF\x01\x01\x01" + b"\0" * 9
    struct.pack_into("<HHIIIIIHHHHHH", header, 16, 2, 40, 1, 0, 0, 52, 0, 52, 0, 0, 40, 1, 0)
    section_header = struct.pack("<10I", 0, 1, 2, 0x1000, 92, len(section), 0, 0, 4, 0)
    return bytes(header) + section_header + section


def optional_apk(
    metadata: bytes = b"pkgname = optional\npkgver = 1-r0\ndepend = so:libshared.so.1\n",
) -> bytes:
    """Include an APKv2 signature stream before its control/data tar stream."""
    streams = []
    for name, data in (
        (".SIGN.RSA.test.pub", b"signature"),
        (".PKGINFO", metadata),
    ):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w") as archive:
            member = tarfile.TarInfo(name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        streams.append(gzip.compress(buffer.getvalue(), mtime=0))
    return b"".join(streams)


def squashfs_header(compression: str) -> bytes:
    """Provide a literal reader fixture, without claiming a mountable filesystem."""
    header = bytearray(96)
    header[:4] = b"hsqs"
    struct.pack_into("<I", header, 12, 65536)
    struct.pack_into("<H", header, 20, {"xz": 4, "lz4": 5}[compression])
    return bytes(header) + b"compressed lower fixture"


class FootprintFixture:
    """A temporary complete artifact set, without a kernel build or package solver."""

    def __init__(self, directory: Path) -> None:
        """Prepare two root packages sharing a library and one hardlinked program."""
        self.directory = directory
        self.database = (
            b"P:app-one\nV:1-r0\nD:so:libshared.so.1\nF:bin\nR:one\nR:one-link\n\n"
            b"P:app-two\nV:1-r0\nD:shared>=1\nF:bin\nR:two\n\n"
            b"P:shared\nV:1-r0\np:so:libshared.so.1=1\nF:lib\nR:shared.so\n\n"
        )

    def bundle(
        self,
        *,
        compression: str = "gzip",
        external: bool = False,
        lower_compression: str = "xz",
        extra: list[tuple[str, bytes, int, int, int]] | None = None,
        extra_apks: dict[str, bytes] | None = None,
    ) -> CurrentBundle:
        """Write actual archives, ELF and matching file records for one report."""
        cpio = newc(
            [
                ("lib/apk/db/installed", self.database, 0o100644, 1, 1),
                ("etc/apk/world", b"app-one\napp-two\n", 0o100644, 2, 1),
                ("bin/one", b"", 0o100755, 3, 2),
                ("bin/one-link", b"12345", 0o100755, 3, 2),
                ("bin/two", b"abc", 0o100755, 4, 1),
                ("lib/shared.so", b"1234567", 0o100755, 5, 1),
                ("bin/link", b"one", 0o120777, 6, 1),
                *(extra or []),
            ]
        )
        self.composition = cpio
        self.lower = squashfs_header(lower_compression)
        self.initramfs = newc(
            [
                ("init", b"#!/bin/sh\n", 0o100755, 1, 1),
                ("root.squashfs", self.lower, 0o100644, 2, 1),
            ]
        )
        payloads = {
            "debug/rootfs.cpio": cpio,
            "debug/kernel.config": (
                b"# CONFIG_BLK_DEV_INITRD is not set\n"
                if external
                else b'CONFIG_INITRAMFS_SOURCE="initramfs.cpio"\n'
            ),
            "debug/zImage": b"kernel-image",
            "debug/vmlinux": kernel_with_initramfs(self.initramfs, compression),
            "debug/System.map": b"00001000 D __initramfs_size\n00001004 D __initramfs_start\n",
            "image/ramboot.bin": b"boot-image",
            "apks/optional.apk": optional_apk(),
            **(extra_apks or {}),
        }
        if not external:
            payloads["debug/initramfs.cpio"] = self.initramfs
        if external:
            payloads["card.img.gz"] = gzip.compress(b"card-image", mtime=0)
            payloads["debug/System.map"] = b"00002000 T kernel_entry\n"
        records = {}
        for name, data in payloads.items():
            path = self.directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o644)
            records[name] = {
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "mode": 0o644,
            }
        manifest = json.dumps(
            {
                "target": "fixture",
                "profile": "microsd-uboot" if external else None,
                "build_type": "release",
                "generation": "a" * 64,
                "files": records,
                "boot_artifacts": {"required": ["card.img.gz"] if external else []},
            }
        ).encode()
        return CurrentBundle(
            self.directory,
            "a" * 64,
            hashlib.sha256(manifest).hexdigest(),
            manifest,
        )

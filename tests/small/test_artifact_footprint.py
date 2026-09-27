# SPDX-License-Identifier: GPL-2.0-only
"""Measure small synthetic bundle artifacts using real archive and ELF bytes."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import lzma
import struct
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any

from fplinux_cli.artifact_footprint import FootprintError, compare_footprints, inspect_footprint
from fplinux_cli.bundle_state import CurrentBundle


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
        payloads = {
            "debug/rootfs.cpio": cpio,
            "debug/kernel.config": (
                b"# CONFIG_BLK_DEV_INITRD is not set\n"
                if external
                else b'CONFIG_INITRAMFS_SOURCE="rootfs.cpio"\n'
            ),
            "debug/zImage": b"kernel-image",
            "debug/vmlinux": kernel_with_initramfs(cpio, compression),
            "debug/System.map": b"00001000 D __initramfs_size\n00001004 D __initramfs_start\n",
            "image/ramboot.bin": b"boot-image",
            "apks/optional.apk": optional_apk(),
            **(extra_apks or {}),
        }
        if external:
            payloads["card.img.gz"] = gzip.compress(b"card-image", mtime=0)
            payloads["debug/System.map"] = b"00002000 T kernel_entry\n"
        records = {}
        for name, data in payloads.items():
            path = self.directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            records[name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
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


class ArtifactFootprintTests(unittest.TestCase):
    """Protect size attribution and comparisons at the host artifact reader boundary."""

    def test_shared_dependency_and_hardlinks_are_counted_once(self) -> None:
        """Consumer chains explain shared bytes without charging them to either app."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            report = inspect_footprint(fixture.bundle())
        root = report["rootfs"]
        packages = root["packages"]
        self.assertEqual(packages["app-one"]["regular_payload_bytes"], 5)
        self.assertEqual(packages["app-two"]["regular_payload_bytes"], 3)
        self.assertEqual(packages["shared"]["regular_payload_bytes"], 7)
        self.assertEqual(
            packages["shared"]["reason_paths"],
            {
                "app-one": ["app-one", "shared"],
                "app-two": ["app-two", "shared"],
            },
        )
        self.assertEqual(root["regular_payload_bytes"], len(fixture.database) + 16 + 15)
        self.assertEqual(root["symlink_payload_bytes"], 3)
        self.assertEqual(root["file_page_model"]["rounded_regular_bytes"], 20480)
        self.assertEqual(root["files"]["/bin/one-link"]["size"], 5)
        self.assertEqual(root["files"]["/bin/one-link"]["accounted_bytes"], 0)
        self.assertEqual(root["unowned_payload_bytes"], len(fixture.database) + 16 + 3)
        self.assertEqual(
            report["optional_apks"]["apks/optional.apk"]["base_dependency_edges"],
            [
                {"requirement": "so:libshared.so.1", "providers": ["shared"]},
            ],
        )
        self.assertNotIn("optional", packages)
        self.assertEqual(
            report["optional_apks"]["apks/optional.apk"]["preinstalled_dependency_packages"],
            ["shared"],
        )

    def test_install_if_explains_automatically_selected_service_package(self) -> None:
        """Both installed triggers explain an automatic subpackage's presence."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            fixture.database += (
                b"P:service\nV:1-r0\ni:app-one app-two\nF:etc/init.d\nR:service\n\n"
            )
            report = inspect_footprint(
                fixture.bundle(
                    extra=[("etc/init.d/service", b"service", 0o100755, 9, 1)],
                )
            )
        package = report["rootfs"]["packages"]["service"]
        self.assertEqual(package["selected_by"], [])
        self.assertEqual(
            package["reason_paths"],
            {
                "app-one": ["app-one", "service"],
                "app-two": ["app-two", "service"],
            },
        )
        self.assertEqual(package["regular_payload_bytes"], 7)

    def test_optional_dependency_chain_keeps_preinstalled_library_in_base(self) -> None:
        """An external add-on inherits an external program's existing base dependencies."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            report = inspect_footprint(
                fixture.bundle(
                    extra_apks={
                        "apks/addon.apk": optional_apk(
                            b"pkgname = addon\npkgver = 1-r0\ndepend = optional\n",
                        ),
                    }
                )
            )
        addon = report["optional_apks"]["apks/addon.apk"]
        self.assertEqual(addon["preinstalled_dependency_packages"], ["shared"])
        self.assertEqual(
            addon["optional_dependency_edges"],
            [
                {"requirement": "optional", "providers": ["optional"]},
            ],
        )
        self.assertEqual(report["rootfs"]["packages"]["shared"]["regular_payload_bytes"], 7)
        self.assertNotIn("addon", report["rootfs"]["packages"])

    def test_compression_change_preserves_content_identity(self) -> None:
        """Actual gzip and XZ streams describe the same files and ownership."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            before = inspect_footprint(fixture.bundle(compression="gzip"))
            after = inspect_footprint(fixture.bundle(compression="xz"))
        delta = compare_footprints(before, after)
        self.assertTrue(delta["same_rootfs_content"])
        self.assertTrue(delta["initramfs_compression_changed"])
        self.assertEqual(delta["files"], {"added": {}, "removed": {}, "changed": {}})
        self.assertEqual(delta["rootfs_byte_delta"]["regular_payload_bytes"], 0)
        self.assertEqual(before["layers"]["embedded_initramfs"]["compression"], "gzip")
        self.assertEqual(after["layers"]["embedded_initramfs"]["compression"], "xz")

    def test_package_and_file_changes_remain_distinct_from_archive_sizes(self) -> None:
        """Version changes and additions have explicit forward and reverse deltas."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            before = inspect_footprint(fixture.bundle())
            fixture.database = fixture.database.replace(b"P:app-two\nV:1-r0", b"P:app-two\nV:2-r0")
            fixture.database += b"P:added\nV:1-r0\nF:bin\nR:new\n\n"
            after = inspect_footprint(fixture.bundle(extra=[("bin/new", b"new!", 0o100755, 9, 1)]))
        delta = compare_footprints(before, after)
        self.assertFalse(delta["same_rootfs_content"])
        self.assertEqual(set(delta["packages"]["added"]), {"added"})
        self.assertEqual(delta["packages"]["changed"]["app-two"]["after"]["version"], "2-r0")
        self.assertEqual(set(delta["files"]["added"]), {"/bin/new"})
        self.assertIn("/lib/apk/db/installed", delta["files"]["changed"])
        reverse = compare_footprints(after, before)
        self.assertEqual(set(reverse["packages"]["removed"]), {"added"})
        self.assertEqual(set(reverse["files"]["removed"]), {"/bin/new"})

    def test_external_root_uses_composition_without_claiming_embedded_initramfs(self) -> None:
        """A storage image and its composition are separate from the boot kernel."""
        with tempfile.TemporaryDirectory() as temporary:
            report = inspect_footprint(FootprintFixture(Path(temporary)).bundle(external=True))
        self.assertIsNone(report["layers"]["embedded_initramfs"])
        self.assertEqual(report["rootfs"]["source"], "external-root-composition")
        self.assertIn("card.img.gz", report["layers"]["boot_artifact_bytes"])
        self.assertIn("debug/rootfs.cpio", report["layers"]["host_debug_file_bytes"])

    def test_changed_artifact_is_rejected_before_reporting_its_size(self) -> None:
        """A stale or incomplete selected bundle cannot yield misleading measurements."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            bundle = fixture.bundle()
            (Path(temporary) / "debug/zImage").write_bytes(b"wrong-kernel")
            with self.assertRaisesRegex(FootprintError, "missing or changed: debug/zImage"):
                inspect_footprint(bundle)

    def test_embedded_payload_must_match_published_composition(self) -> None:
        """Valid individual file hashes alone do not prove the embedded composition."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            bundle = fixture.bundle()
            elf = kernel_with_initramfs(newc([]), "gzip")
            (Path(temporary) / "debug/vmlinux").write_bytes(elf)
            manifest: dict[str, Any] = json.loads(bundle.manifest_bytes)
            manifest["files"]["debug/vmlinux"] = {
                "size": len(elf),
                "sha256": hashlib.sha256(elf).hexdigest(),
            }
            replaced = CurrentBundle(
                bundle.path,
                bundle.generation,
                "",
                json.dumps(manifest).encode(),
            )
            with self.assertRaisesRegex(FootprintError, "differs from the published root"):
                inspect_footprint(replaced)


if __name__ == "__main__":
    unittest.main()

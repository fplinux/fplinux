# SPDX-License-Identifier: GPL-2.0-only
"""Measure small synthetic bundle artifacts using real archive and ELF bytes."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from typing import Any

from fplinux_cli.artifact_footprint import FootprintError, compare_footprints, inspect_footprint
from fplinux_cli.bundle_state import CurrentBundle

from tests.fixtures.artifact_footprint import (
    FootprintFixture,
    kernel_with_initramfs,
    optional_apk,
)


class ArtifactFootprintTests(unittest.TestCase):
    """Protect size attribution and comparisons at the host artifact reader boundary."""

    def test_compressed_lower_reports_its_own_bytes_without_changing_attribution(self) -> None:
        """Boot backing size and package ownership describe separate archive layers."""
        for compression in ("xz", "lz4"):
            with self.subTest(compression=compression), tempfile.TemporaryDirectory() as temporary:
                fixture = FootprintFixture(Path(temporary))
                report = inspect_footprint(fixture.bundle(lower_compression=compression))
            lower = report["layers"]["ram_root"]
            self.assertEqual(lower["filesystem"], "squashfs")
            self.assertEqual(lower["compression"], compression)
            self.assertEqual(lower["block_bytes"], 65536)
            self.assertEqual(lower["bytes"], 120)
            self.assertEqual(lower["sha256"], hashlib.sha256(fixture.lower).hexdigest())
            self.assertEqual(report["rootfs"]["source"], "ram-squashfs-composition")
            self.assertEqual(report["rootfs"]["packages"]["app-one"]["regular_payload_bytes"], 5)
            self.assertEqual(
                report["layers"]["embedded_initramfs"]["cpio_bytes"], len(fixture.initramfs)
            )
            self.assertNotEqual(len(fixture.initramfs), len(fixture.composition))

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

    def test_package_and_file_changes_have_forward_and_reverse_deltas(self) -> None:
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

    def test_embedded_payload_must_match_published_boot_archive(self) -> None:
        """Valid file hashes alone do not establish which boot archive the ELF embeds."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = FootprintFixture(Path(temporary))
            bundle = fixture.bundle()
            elf = kernel_with_initramfs(fixture.composition, "gzip")
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

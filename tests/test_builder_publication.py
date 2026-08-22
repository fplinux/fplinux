# SPDX-License-Identifier: GPL-2.0-only
"""Focused builder tests for atomic immutable bundle publication."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from typing import cast
from unittest import mock

from fplinux_cli import alpine_state, builder
from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    bundle_generations,
    create_bundle_staging,
    resolve_current_bundle,
)
from fplinux_cli.common import sha256_file


class BuilderPublicationTests(unittest.TestCase):
    """Exercise builder-facing immutable bundle publication."""

    def setUp(self) -> None:
        """Create a complete isolated builder input tree."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.output = Path(self.temporary.name) / "out"
        self.work = Path(self.temporary.name) / "work"
        self.root.mkdir()
        self.output.mkdir()
        self.work.mkdir()
        self.write("common/run.py", b"#!/usr/bin/env python3\n")
        self.write("platforms/demo/host/adapter.py", b"ADAPTER = 'demo'\n")
        self.write("THIRD_PARTY_NOTICES.md", b"notices\n")
        self.asset_lock = self.write("assets.lock.toml", b"[asset]\n")
        self.write("assets/pin.bin", b"asset\n", root=self.work)
        self.host_tool = self.write("console", b"host tool\n", root=self.work)
        self.kernel = self.work / "kernel"
        self.kernel.mkdir()
        self.zimage = self.write("zImage", b"zimage\n", root=self.kernel)
        self.dtb = self.write("demo.dtb", b"dtb\n", root=self.kernel)
        self.write("vmlinux", b"vmlinux\n", root=self.kernel)
        self.write("System.map", b"map\n", root=self.kernel)
        self.write(".config", b"CONFIG_TEST=y\n", root=self.kernel)
        self.ramboot = self.write("ramboot.bin", b"ramboot\n", root=self.work)
        self.ramboot_map = self.write("ramboot.map", b"map\n", root=self.work)
        self.rootfs_output = self.work / "rootfs"
        self.rootfs_recipe = "d" * 64
        self.rootfs = self.write("rootfs.cpio", b"rootfs\n", root=self.rootfs_output)
        self.bundle_apks = {
            "fplinux-phone-ui": self.write(
                "packages/fplinux-phone-ui-1-r0.apk",
                b"base apk\n",
                root=self.work,
            ),
            "fplinux-phone-ui-demo": self.write(
                "packages/fplinux-phone-ui-demo-3-r1.apk",
                b"target apk\n",
                root=self.work,
            ),
        }
        self.target_config = {
            "profile": "default",
            "display_name": "Demo",
            "platform": "demo",
            "bundle": {"packages": list(self.bundle_apks)},
            "linux": {"debug_dtb": "demo.dtb"},
            "bootstrap": {"load_address": 2},
            "runtime": {
                "assets": {"pin": "assets/pin.bin"},
                "fdl1_load_address": 1,
                "usb": {"transport": "test"},
                "adapter": {"kind": "test"},
            },
        }
        self.platform = {
            "bundle": {"packages": []},
            "linux": {"cross_compile": "arm-"},
            "host": {
                "runtime_tools": {"console": "console"},
                "capability": "test",
            },
        }
        self.release_manifest = {
            "image": "image/ramboot.bin",
            "bundle_files": [
                "image/ramboot.bin",
                "assets/pin.bin",
                "host/console",
                "runner/run.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
                "assets.lock.toml",
                "THIRD_PARTY_NOTICES.md",
                "apks/fplinux-phone-ui.apk",
                "apks/fplinux-phone-ui-demo.apk",
            ],
        }
        self.environment = {
            "FPLINUX_WORKSPACE_DIGEST": "a" * 64,
            "FPLINUX_CONTAINER_IMAGE_RECIPE": "b" * 64,
        }
        self.root_patch = mock.patch.object(builder, "ROOT", self.root)
        self.output_patch = mock.patch.object(builder, "OUTPUT", self.output)
        self.root_patch.start()
        self.output_patch.start()
        self.receipt_patch = mock.patch.object(
            alpine_state,
            "trusted_receipt_identity",
            return_value={"recipe": self.rootfs_recipe, "sha256": "8" * 64},
        )
        self.signing_patch = mock.patch.object(
            alpine_state,
            "signing_key_identity",
            return_value="7" * 64,
        )
        self.receipt_patch.start()
        self.signing_patch.start()
        self.addCleanup(self.signing_patch.stop)
        self.addCleanup(self.receipt_patch.stop)
        self.addCleanup(self.output_patch.stop)
        self.addCleanup(self.root_patch.stop)

    def write(self, relative: str, data: bytes, *, root: Path | None = None) -> Path:
        """Write one fixture file beneath the requested test root."""
        if root is None:
            root = self.root
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def publish(
        self,
        ramboot: Path | None = None,
        bundle_packages: tuple[str, ...] | None = None,
    ) -> Path:
        """Publish the configured fixture through the builder entry point."""
        with mock.patch.dict(os.environ, self.environment, clear=False):
            return builder.publish_bundle(
                "demo",
                self.target_config,
                self.platform,
                self.release_manifest,
                self.work,
                self.rootfs,
                self.kernel,
                self.zimage,
                self.dtb,
                self.ramboot if ramboot is None else ramboot,
                self.ramboot_map,
                self.asset_lock,
                {"pin": ("pin.bin", sha256_file(self.work / "assets/pin.bin"))},
                {"console": self.host_tool},
                "c" * 64,
                "9" * 64,
                self.rootfs_output,
                self.rootfs_recipe,
                {"recipe": "e" * 64, "sha256": "f" * 64},
                tuple(self.bundle_apks) if bundle_packages is None else bundle_packages,
                self.bundle_apks,
            )

    def staging_directories(self) -> list[Path]:
        """Return only incomplete staging trees for this test target/profile."""
        generations = bundle_generations(self.output, "demo", "default")
        if not generations.exists():
            return []
        return sorted(path for path in generations.iterdir() if path.name.startswith(".stage-"))

    def assert_old_current_and_no_staging(self, current: Path) -> None:
        """Check failure cleanup without disturbing the last-good selected generation."""
        self.assertEqual(
            resolve_current_bundle(self.output, "demo", "default").path,
            current,
        )
        self.assertEqual(self.staging_directories(), [])

    def test_publish_uses_staging_and_records_every_payload_file(self) -> None:
        """Publish a complete generation through the current pointer."""
        published = self.publish()
        current = resolve_current_bundle(self.output, "demo", "default")
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        runtime = json.loads((published / "runtime-manifest.json").read_text())

        self.assertEqual(current.path, published)
        self.assertEqual(
            published.parent,
            self.output / "demo/bundles/default",
        )
        self.assertEqual(
            set(manifest),
            {
                "rootfs_receipt",
                "container_image_recipe",
                "apk_signing_key",
                "device_identity",
                "files",
                "generation",
                "kbuild_receipt",
                "linux_recipe",
                "profile",
                "target",
                "workspace_digest",
            },
        )
        self.assertEqual(manifest["generation"], published.name)
        self.assertEqual(manifest["apk_signing_key"], "7" * 64)
        self.assertEqual(manifest["rootfs_receipt"]["recipe"], self.rootfs_recipe)
        self.assertEqual(manifest["kbuild_receipt"]["recipe"], "e" * 64)
        self.assertEqual(manifest["device_identity"], "9" * 64)
        self.assertIn("debug/rootfs.cpio", manifest["files"])
        self.assertIn("debug/vmlinux", manifest["files"])
        self.assertEqual(
            (published / "apks/fplinux-phone-ui.apk").read_bytes(),
            b"base apk\n",
        )
        self.assertEqual(
            (published / "apks/fplinux-phone-ui-demo.apk").read_bytes(),
            b"target apk\n",
        )
        self.assertIn("apks/fplinux-phone-ui.apk", manifest["files"])
        self.assertIn("apks/fplinux-phone-ui-demo.apk", manifest["files"])
        self.assertEqual(
            manifest["files"]["debug/rootfs.cpio"]["size"],
            self.rootfs.stat().st_size,
        )
        self.assertNotIn(BUILD_MANIFEST_NAME, manifest["files"])
        self.assertEqual(
            runtime["sha256"]["image/ramboot.bin"],
            sha256_file(published / "image/ramboot.bin"),
        )

    def test_publish_rejects_apks_outside_the_declared_bundle_package_set(self) -> None:
        """A bundle cannot silently add or omit one of its declared packages."""
        current = self.publish()

        with self.assertRaisesRegex(SystemExit, "declared bundle package set"):
            self.publish(bundle_packages=("fplinux-phone-ui",))

        self.assert_old_current_and_no_staging(current)

    def test_publish_rejects_release_manifest_apks_outside_the_declared_set(self) -> None:
        """The release manifest cannot omit or add an independently named APK."""
        current = self.publish()
        bundle_files = cast("list[str]", self.release_manifest["bundle_files"])
        bundle_files.remove("apks/fplinux-phone-ui-demo.apk")

        with self.assertRaisesRegex(SystemExit, "release manifest APK files"):
            self.publish()

        self.assert_old_current_and_no_staging(current)

    def test_copy_failure_preserves_the_current_bundle_and_cleans_staging(self) -> None:
        """Retain the current bundle when copying a new payload fails."""
        first = self.publish()
        foreign_staging = create_bundle_staging(self.output, "demo", "default")
        missing = self.work / "missing-ramboot.bin"

        with self.assertRaises(SystemExit):
            self.publish(missing)

        self.assertEqual(
            resolve_current_bundle(self.output, "demo", "default").path,
            first,
        )
        self.assertEqual(self.staging_directories(), [foreign_staging])

    def test_manifest_failure_preserves_the_current_bundle_and_cleans_staging(self) -> None:
        """Retain the current bundle when writing its new manifest fails."""
        first = self.publish()
        real_write_json = builder.write_json
        message = "manifest write failed"

        def write_json(path: Path, value: dict[str, object], *, prefix: str) -> None:
            if path.name == BUILD_MANIFEST_NAME:
                raise OSError(message)
            real_write_json(path, value, prefix=prefix)

        with (
            mock.patch.object(builder, "write_json", side_effect=write_json),
            self.assertRaisesRegex(OSError, message),
        ):
            self.publish()

        self.assert_old_current_and_no_staging(first)


if __name__ == "__main__":
    unittest.main()

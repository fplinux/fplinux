# SPDX-License-Identifier: GPL-2.0-only
"""Focused builder tests for immutable bundle publication."""

from __future__ import annotations

import hashlib
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path
from typing import Any, cast
from unittest import mock

from fplinux_cli import alpine_state, builder
from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    bundle_generations,
    create_bundle_staging,
    resolve_current_bundle,
)


class ProfileKconfigTests(unittest.TestCase):
    """Check the profile-only Kconfig actions consumed by the Kbuild plan."""

    def test_actions_are_normalized_and_must_survive_olddefconfig(self) -> None:
        """The final .config, rather than the requested input, is the build oracle."""
        with tempfile.TemporaryDirectory() as temporary:
            config = Path(temporary) / ".config"
            config.write_text("CONFIG_PROFILE_ENABLED=y\n# CONFIG_PROFILE_DISABLED is not set\n")

            self.assertEqual(
                builder.profile_kconfig_arguments(
                    ["CONFIG_PROFILE_ENABLED"], ["CONFIG_PROFILE_DISABLED"]
                ),
                [
                    "--enable",
                    "PROFILE_ENABLED",
                    "--disable",
                    "PROFILE_DISABLED",
                ],
            )
            builder.assert_profile_kconfig(
                config,
                ["CONFIG_PROFILE_ENABLED"],
                ["CONFIG_PROFILE_DISABLED"],
            )

            config.write_text("# CONFIG_PROFILE_ENABLED is not set\n")
            with self.assertRaisesRegex(SystemExit, "profile did not enable"):
                builder.assert_profile_kconfig(config, ["CONFIG_PROFILE_ENABLED"], [])


class RamSessionImageTests(unittest.TestCase):
    """Exercise the canonical static-image personalization boundary."""

    @staticmethod
    def fdt(
        chosen_properties: list[tuple[str, bytes]],
        session_properties: list[tuple[str, bytes]],
    ) -> bytes:
        """Build one minimal independent /chosen plus /fplinux-session fixture."""
        names = b""
        offsets: dict[str, int] = {}
        for properties in (chosen_properties, session_properties):
            for name, _value in properties:
                if name not in offsets:
                    offsets[name] = len(names)
                    names += name.encode("ascii") + b"\0"

        structure = bytearray()

        def word(value: int) -> None:
            structure.extend(struct.pack(">I", value))

        def align() -> None:
            structure.extend(bytes(-len(structure) % 4))

        word(1)
        structure.extend(b"\0")
        align()
        for node, properties in (
            ("chosen", chosen_properties),
            ("fplinux-session", session_properties),
        ):
            word(1)
            structure.extend(node.encode("ascii") + b"\0")
            align()
            for name, value in properties:
                word(3)
                word(len(value))
                word(offsets[name])
                structure.extend(value)
                align()
            word(2)
        word(2)
        word(9)

        reserved_offset = 40
        structure_offset = reserved_offset + 16
        strings_offset = structure_offset + len(structure)
        total_size = strings_offset + len(names)
        header = struct.pack(
            ">10I",
            0xD00DFEED,
            total_size,
            structure_offset,
            strings_offset,
            reserved_offset,
            17,
            16,
            0,
            len(names),
            len(structure),
        )
        return header + bytes(16) + structure + names

    def image_fixture(self) -> tuple[Path, Path, Path, Path, int, int]:
        """Create one RAM image with the exact externally defined ABI."""
        root = Path(self.temporary.name)
        load_address = 0x80100000
        kernel = bytearray(0x28)
        kernel[0x24:0x28] = b"\x18\x28\x6f\x01"
        tree = self.fdt(
            [
                ("rng-seed", bytes([0xA1]) * 64),
            ],
            [
                ("compatible", b"fplinux,ram-session\0"),
                ("fplinux,ssh-client-key", bytes([0xB2]) * 68),
                ("fplinux,session-id", bytes([0xC3]) * 32),
                ("fplinux,usb-session", bytes([0xD4]) * 256),
            ],
        )
        zimage_offset = 0x200
        dtb_offset = (zimage_offset + len(kernel) + 63) & ~63
        session_offset = (dtb_offset + len(tree) + 63) & ~63
        image = bytearray(session_offset + 512)
        image[:4] = b"DHTB"
        struct.pack_into("<I", image, 0x30, len(image) - 0x200)
        image[zimage_offset : zimage_offset + len(kernel)] = kernel
        image[dtb_offset : dtb_offset + len(tree)] = tree

        ramboot = root / "ramboot.bin"
        zimage = root / "zImage"
        dtb = root / "target.dtb"
        map_file = root / "ramboot.map"
        ramboot.write_bytes(image)
        zimage.write_bytes(kernel)
        dtb.write_bytes(tree)
        symbols = {
            "__image_start": load_address,
            "linux_zimage_start": load_address + zimage_offset,
            "linux_zimage_end": load_address + zimage_offset + len(kernel),
            "linux_dtb_start": load_address + dtb_offset,
            "linux_dtb_end": load_address + dtb_offset + len(tree),
            "fplinux_session_start": load_address + session_offset,
            "fplinux_session_end": load_address + session_offset + 512,
            "FPLINUX_BOOTSTRAP_STORAGE_DISABLED": 1,
        }
        map_file.write_text("".join(f"{value:08x} T {name}\n" for name, value in symbols.items()))
        return (
            ramboot,
            zimage,
            dtb,
            map_file,
            load_address,
            session_offset,
        )

    def setUp(self) -> None:
        """Create one private artifact directory per scenario."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)

    def test_session_descriptor_locates_an_immutable_zero_template(self) -> None:
        """Describe the exact slot without personalizing the built artifact."""
        ramboot, zimage, dtb, map_file, load_address, offset = self.image_fixture()

        descriptor = builder.verify_images(
            ramboot,
            zimage,
            dtb,
            map_file,
            load_address,
            load_address + ramboot.stat().st_size + 1,
            [],
        )

        self.assertEqual(
            descriptor,
            {
                "offset": offset,
                "bytes": 512,
                "template_sha256": hashlib.sha256(bytes(512)).hexdigest(),
            },
        )
        self.assertEqual(ramboot.read_bytes()[offset : offset + 512], bytes(512))

    def test_session_descriptor_rejects_a_prepersonalized_image(self) -> None:
        """Never publish key material already embedded in the RAM image."""
        ramboot, zimage, dtb, map_file, load_address, offset = self.image_fixture()
        image = bytearray(ramboot.read_bytes())
        image[offset] = 1
        ramboot.write_bytes(image)

        with self.assertRaisesRegex(SystemExit, "not all zero"):
            builder.verify_images(
                ramboot,
                zimage,
                dtb,
                map_file,
                load_address,
                load_address + len(image) + 1,
                [],
            )


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
        self.write("scripts/fplinux_cli/identity.py", b"# identity contract\n")
        self.write("scripts/fplinux_cli/ssh_transport.py", b"# bundled SSH helper\n")
        self.write("platforms/demo/host/adapter.py", b"ADAPTER = 'demo'\n")
        self.write("THIRD_PARTY_NOTICES.md", b"notices\n")
        self.asset_lock = self.write("assets.lock.toml", b"[asset]\n")
        self.write("assets/pin.bin", b"asset\n", root=self.work)
        self.host_tool = self.write("keyboard", b"host tool\n", root=self.work)
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
            "fplinux-base-ui": self.write(
                "packages/fplinux-base-ui-1-r0.apk",
                b"base apk\n",
                root=self.work,
            ),
            "fplinux-demo-ui": self.write(
                "packages/fplinux-demo-ui-3-r1.apk",
                b"target apk\n",
                root=self.work,
            ),
        }
        self.target_config: dict[str, Any] = {
            "identity": {
                "brand": "Demo",
                "product": "Phone",
                "hardware_codes": [],
                "compatible": "demo,phone",
                "display_name": "Demo Phone",
            },
            "platform": "demo",
            "bundle": {"packages": list(self.bundle_apks)},
            "linux": {"debug_dtb": "demo.dtb", "root": {"kind": "initramfs"}},
            "bootstrap": {"load_address": 2},
            "runtime": {
                "fdl1_load_address": 1,
                "usb": {"kind": "test"},
                "adapter": {"kind": "test"},
            },
        }
        self.platform = {
            "identity": {
                "vendor": "Demo",
                "soc": "SOC1",
                "aliases": [],
                "compatible": "demo,soc1",
                "display_name": "Demo SOC1",
            },
            "bundle": {"packages": []},
            "linux": {"cross_compile": "arm-"},
            "host": {
                "runtime_tools": {"keyboard": "keyboard"},
            },
        }
        self.release_manifest = {
            "image": "image/ramboot.bin",
            "bundle_files": [
                "image/ramboot.bin",
                "assets/pin.bin",
                "host/keyboard",
                "runner/run.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
                "assets.lock.toml",
                "THIRD_PARTY_NOTICES.md",
                "apks/fplinux-base-ui.apk",
                "apks/fplinux-demo-ui.apk",
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
        personalization: dict[str, int | str] | None = None,
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
                (
                    {
                        "offset": 1024,
                        "bytes": 512,
                        "template_sha256": hashlib.sha256(bytes(512)).hexdigest(),
                    }
                    if personalization is None
                    else personalization
                ),
                self.asset_lock,
                {
                    "pin": (
                        "pin.bin",
                        hashlib.sha256((self.work / "assets/pin.bin").read_bytes()).hexdigest(),
                    )
                },
                {"keyboard": self.host_tool},
                "c" * 64,
                "9" * 64,
                self.rootfs_output,
                self.rootfs_recipe,
                {"recipe": "e" * 64, "sha256": "f" * 64},
                tuple(self.bundle_apks) if bundle_packages is None else bundle_packages,
                self.bundle_apks,
            )

    def staging_directories(self) -> list[Path]:
        """Return only incomplete staging trees for this test target."""
        generations = bundle_generations(self.output, "demo")
        if not generations.exists():
            return []
        return sorted(path for path in generations.iterdir() if path.name.startswith(".stage-"))

    def assert_old_current_and_no_staging(self, current: Path) -> None:
        """Check failure cleanup without disturbing the last-good selected generation."""
        self.assertEqual(
            resolve_current_bundle(self.output, "demo").path,
            current,
        )
        self.assertEqual(self.staging_directories(), [])

    def test_publish_uses_staging_and_records_every_payload_file(self) -> None:
        """Publish a complete generation through the current pointer."""
        published = self.publish()
        current = resolve_current_bundle(self.output, "demo")
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        runtime = json.loads((published / "runtime-manifest.json").read_text())
        actual_payload = {
            path.relative_to(published).as_posix(): {
                "mode": path.stat().st_mode & 0o777,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "size": path.stat().st_size,
            }
            for path in published.rglob("*")
            if path.is_file() and path.name != BUILD_MANIFEST_NAME
        }

        self.assertEqual(current.path, published)
        self.assertEqual(
            published.parent,
            self.output / "demo/bundles",
        )
        self.assertEqual(
            set(manifest),
            {
                "rootfs_receipt",
                "boot_artifacts",
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
        self.assertIsNone(manifest["profile"])
        self.assertEqual(manifest["apk_signing_key"], "7" * 64)
        self.assertEqual(manifest["rootfs_receipt"]["recipe"], self.rootfs_recipe)
        self.assertEqual(
            manifest["boot_artifacts"],
            {
                "required": [],
                "runnable": True,
            },
        )
        self.assertEqual(manifest["kbuild_receipt"]["recipe"], "e" * 64)
        self.assertEqual(manifest["device_identity"], "9" * 64)
        self.assertEqual(manifest["files"], actual_payload)
        self.assertEqual(
            (published / "apks/fplinux-base-ui.apk").read_bytes(),
            b"base apk\n",
        )
        self.assertEqual(
            (published / "apks/fplinux-demo-ui.apk").read_bytes(),
            b"target apk\n",
        )
        self.assertEqual(
            manifest["files"]["debug/rootfs.cpio"]["size"],
            self.rootfs.stat().st_size,
        )
        self.assertEqual(
            runtime["sha256"]["image/ramboot.bin"],
            hashlib.sha256((published / "image/ramboot.bin").read_bytes()).hexdigest(),
        )
        self.assertEqual(
            set(runtime),
            {
                "target",
                "profile",
                "identity",
                "transport",
                "image",
                "addresses",
                "usb",
                "personalization",
                "assets",
                "adapter",
                "host_tools",
                "sha256",
            },
        )
        self.assertIsNone(runtime["profile"])
        self.assertEqual(runtime["identity"]["target"]["display_name"], "Demo Phone")
        self.assertEqual(runtime["identity"]["platform"]["name"], "demo")
        self.assertEqual(runtime["transport"], "usb-ncm")
        helper = published / "runner/ssh_transport.py"
        self.assertEqual(runtime["personalization"]["bytes"], 512)
        self.assertEqual(runtime["assets"], {"pin": "assets/pin.bin"})
        self.assertEqual(
            runtime["sha256"]["runner/ssh_transport.py"],
            hashlib.sha256(helper.read_bytes()).hexdigest(),
        )

    def test_publication_requires_the_hashed_ssh_helper(self) -> None:
        """Reject a runtime closure which omits the mandatory SSH helper."""
        descriptor: dict[str, int | str] = {
            "offset": 1024,
            "bytes": 512,
            "template_sha256": hashlib.sha256(bytes(512)).hexdigest(),
        }

        helper = self.root / "scripts/fplinux_cli/ssh_transport.py"
        helper.unlink()
        with self.assertRaisesRegex(SystemExit, "expected file is missing or invalid"):
            self.publish(personalization=descriptor)

    def test_named_profile_publishes_to_its_own_current_bundle_slot(self) -> None:
        """A host profile cannot replace the default target bundle pointer."""
        self.target_config["profile"] = "usb-host-lab"
        self.target_config["runtime"]["transport"] = "none"

        published = self.publish()
        current = resolve_current_bundle(self.output, "demo", "usb-host-lab")
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        runtime = json.loads((published / "runtime-manifest.json").read_text())

        self.assertEqual(published.parent, self.output / "demo/profiles/usb-host-lab/bundles")
        self.assertEqual(current.path, published)
        self.assertEqual(manifest["profile"], "usb-host-lab")
        self.assertEqual(runtime["profile"], "usb-host-lab")
        self.assertEqual(runtime["transport"], "none")

    def test_external_root_bundle_does_not_copy_an_unused_initramfs(self) -> None:
        """A profile whose kernel boots ext4 omits the unrelated cpio debug copy."""
        self.target_config["profile"] = "microsd"
        self.target_config["linux"]["root"] = {
            "kind": "external",
            "filesystem": "ext4",
            "partuuid": "46504c58-02",
            "wait_seconds": 10,
        }

        published = self.publish()

        self.assertFalse((published / "debug/rootfs.cpio").exists())
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        self.assertNotIn("debug/rootfs.cpio", manifest["files"])

    def test_publish_rejects_apks_outside_the_declared_bundle_package_set(self) -> None:
        """A bundle cannot silently add or omit one of its declared packages."""
        current = self.publish()

        with self.assertRaisesRegex(SystemExit, "declared bundle package set"):
            self.publish(bundle_packages=("fplinux-base-ui",))

        self.assert_old_current_and_no_staging(current)

    def test_publish_rejects_release_manifest_apks_outside_the_declared_set(self) -> None:
        """The release manifest cannot omit or add an independently named APK."""
        current = self.publish()
        bundle_files = cast("list[str]", self.release_manifest["bundle_files"])
        bundle_files.remove("apks/fplinux-demo-ui.apk")

        with self.assertRaisesRegex(SystemExit, "release manifest APK files"):
            self.publish()

        self.assert_old_current_and_no_staging(current)

    def test_copy_failure_preserves_the_current_bundle_and_cleans_staging(self) -> None:
        """Retain the current bundle when copying a new payload fails."""
        first = self.publish()
        create_bundle_staging(self.output, "demo")
        missing = self.work / "missing-ramboot.bin"

        with self.assertRaises(SystemExit):
            self.publish(missing)

        self.assertEqual(
            resolve_current_bundle(self.output, "demo").path,
            first,
        )
        self.assertEqual(self.staging_directories(), [])

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

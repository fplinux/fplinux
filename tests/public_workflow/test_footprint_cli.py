# SPDX-License-Identifier: GPL-2.0-only
"""Measure synthetic artifacts and compare saved reports through the public CLI."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.bundles import (
    create_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
)

from tests.cli_support import prepare_cli_checkout
from tests.fixtures.artifact_footprint import (
    FootprintFixture,
    kernel_with_initramfs,
    squashfs_header,
)
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess


class FootprintCliTests(unittest.TestCase):
    """Use real archives and the public resolver without a build runtime or phone."""

    def setUp(self) -> None:
        """Keep commands and artifacts in an isolated checkout."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        prepare_cli_checkout(self.root)

    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Run the actual public entry point with a bounded lifetime."""
        return run_process(
            [str(self.root / "fplinux"), *arguments],
            name="public footprint inspection",
            cwd=self.root,
            timeout=10,
        )

    def publish_bundle(self, *, profile: str | None = None, build_type: str = "release") -> Path:
        """Publish independent archive fixtures with a complete current-bundle manifest."""
        output = self.root / ".cache/out"
        output.mkdir(parents=True, exist_ok=True)
        staging = create_bundle_staging(output, "example", profile, build_type=build_type)
        bundle = FootprintFixture(staging).bundle(external=profile is not None)
        manifest = json.loads(bundle.manifest_bytes)
        manifest.update(
            target="example",
            profile=profile,
            build_type=build_type,
            workspace_digest="a" * 64,
            container_image_recipe="b" * 64,
            container_image_content="c" * 64,
            apk_signing_key="d" * 64,
            device_identity="e" * 64,
            linux_recipe="f" * 64,
            rootfs_receipt={},
            kbuild_receipt={},
        )
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        published = publish_bundle_generation(
            output, "example", staging, bundle.generation, profile, build_type=build_type
        )
        publish_current_bundle(output, "example", published, profile, build_type=build_type)
        return published

    def test_footprint_selects_exact_profile_and_build_type_in_json_and_text(self) -> None:
        """Readable and machine reports retain the selected identity and measured bytes."""
        self.publish_bundle()
        self.publish_bundle(build_type="debug")
        self.publish_bundle(profile="microsd-uboot")
        for profile, build_type in (
            (None, "release"),
            (None, "debug"),
            ("microsd-uboot", "release"),
        ):
            with self.subTest(profile=profile, build_type=build_type):
                arguments = ["inspect", "footprint", "example", "--build-type", build_type]
                if profile is not None:
                    arguments.extend(["--profile", profile])
                result = self.run_cli(*arguments, "--json")
                self.assertEqual(result.returncode, 0, result.stderr)
                report = json.loads(result.stdout)
                self.assertEqual(report["identity"]["target"], "example")
                self.assertEqual(report["identity"]["profile"], profile)
                self.assertEqual(report["identity"]["build_type"], build_type)
                self.assertEqual(report["layers"]["kernel_zimage_bytes"], 12)
                if profile is None:
                    self.assertEqual(report["rootfs"]["source"], "ram-squashfs-composition")
                    self.assertEqual(report["layers"]["ram_root"]["compression"], "xz")
                    self.assertEqual(report["layers"]["ram_root"]["block_bytes"], 65536)
                    self.assertEqual(report["layers"]["ram_root"]["bytes"], 120)
                    self.assertEqual(
                        report["layers"]["ram_root"]["sha256"],
                        hashlib.sha256(squashfs_header("xz")).hexdigest(),
                    )
                    self.assertEqual(report["layers"]["embedded_initramfs"]["compression"], "gzip")
                else:
                    self.assertIsNone(report["layers"]["ram_root"])
                    self.assertIsNone(report["layers"]["embedded_initramfs"])
                self.assertEqual(
                    report["rootfs"]["packages"]["shared"]["regular_payload_bytes"], 7
                )
                self.assertEqual(report["rootfs"]["files"]["/bin/one-link"]["accounted_bytes"], 0)
                text = self.run_cli(*arguments)
                self.assertEqual(text.returncode, 0, text.stderr)
                self.assertIn("Kernel zImage: 12 B", text.stdout)
                self.assertIn("7 B  shared 1-r0", text.stdout)
                self.assertIn("Nested layers overlap", text.stdout)
                self.assertNotIn(str(self.root), text.stdout)

    def test_missing_build_type_fails_without_measurements(self) -> None:
        """A release bundle cannot satisfy a request for a debug report."""
        self.publish_bundle()
        missing = self.run_cli(
            "inspect", "footprint", "example", "--build-type", "debug", "--json"
        )
        self.assertEqual(missing.returncode, 1, missing.stderr)
        self.assertIn("build example --build-type debug", missing.stderr)
        self.assertEqual(missing.stdout, "")

    def test_missing_payload_fails_without_partial_report_or_traceback(self) -> None:
        """A selected RAM bundle must retain both composition and boot archives."""
        published = self.publish_bundle()
        for payload in ("debug/rootfs.cpio", "debug/initramfs.cpio"):
            with self.subTest(payload=payload):
                path = published / payload
                original = path.read_bytes()
                path.unlink()
                try:
                    result = self.run_cli("inspect", "footprint", "example", "--json")
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertEqual(result.stdout, "")
                    self.assertIn(payload, result.stderr)
                    self.assertNotIn("Traceback", result.stderr)
                finally:
                    path.write_bytes(original)

    def test_kernel_embedding_composition_instead_of_boot_archive_is_rejected(self) -> None:
        """Individually valid artifacts cannot report the wrong embedded boot content."""
        output = self.root / ".cache/out"
        output.mkdir(parents=True)
        staging = create_bundle_staging(output, "example")
        fixture = FootprintFixture(staging)
        bundle = fixture.bundle()
        elf = kernel_with_initramfs(fixture.composition, "gzip")
        (staging / "debug/vmlinux").write_bytes(elf)
        manifest = json.loads(bundle.manifest_bytes)
        manifest.update(
            target="example",
            profile=None,
            build_type="release",
            workspace_digest="a" * 64,
            container_image_recipe="b" * 64,
            container_image_content="c" * 64,
            apk_signing_key="d" * 64,
            device_identity="e" * 64,
            linux_recipe="f" * 64,
            rootfs_receipt={},
            kbuild_receipt={},
        )
        manifest["files"]["debug/vmlinux"] = {
            "size": len(elf),
            "sha256": hashlib.sha256(elf).hexdigest(),
            "mode": 0o644,
        }
        (staging / "build-manifest.json").write_text(json.dumps(manifest))
        published = publish_bundle_generation(output, "example", staging, bundle.generation)
        publish_current_bundle(output, "example", published)
        result = self.run_cli("inspect", "footprint", "example", "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("embedded initramfs differs", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def write_reports(self) -> None:
        """Save small independent measurements with a known kernel and package delta."""
        before: dict[str, Any] = {
            "identity": {"target": "example", "generation": "before"},
            "layers": {
                "kernel_zimage_bytes": 1024,
                "boot_artifact_bytes": {"image/ramboot.bin": 2048},
                "embedded_initramfs": {"compression": "gzip"},
            },
            "rootfs": {
                "content_sha256": "a" * 64,
                "cpio_bytes": 512,
                "regular_payload_bytes": 100,
                "symlink_payload_bytes": 0,
                "packages": {"example": {"version": "1-r0"}},
                "files": {"/bin/example": {"size": 100}},
            },
            "optional_apks": {},
        }
        (self.root / "before.json").write_text(json.dumps(before))
        before["layers"]["kernel_zimage_bytes"] = 2048
        before["rootfs"]["packages"]["example"]["version"] = "2-r0"
        (self.root / "after.json").write_text(json.dumps(before))

    def test_diff_reports_saved_deltas_without_creating_cache(self) -> None:
        """Comparing saved measurements needs neither bundles nor build state."""
        self.write_reports()
        result = self.run_cli("inspect", "footprint-diff", "before.json", "after.json", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["kernel_zimage_byte_delta"], 1024)
        self.assertEqual(report["packages"]["changed"]["example"]["after"]["version"], "2-r0")
        self.assertTrue(report["same_rootfs_content"])
        result = self.run_cli("inspect", "footprint-diff", "before.json", "after.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Kernel zImage delta: 1,024 B (1.0 KiB)", result.stdout)
        self.assertIn("changed: example", result.stdout)
        self.assertFalse((self.root / ".cache").exists())

    def test_diff_rejects_missing_invalid_and_unrelated_json_without_traceback(self) -> None:
        """Ordinary input mistakes yield CLI errors rather than partial comparisons."""
        self.write_reports()
        for name, content in (
            ("invalid.json", "broken"),
            ("array.json", "[]"),
            ("empty.json", "{}"),
        ):
            (self.root / name).write_text(content)
        for name in ("missing.json", "invalid.json", "array.json", "empty.json"):
            with self.subTest(name=name):
                result = self.run_cli("inspect", "footprint-diff", name, "after.json", "--json")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("Traceback", result.stderr)
        self.assertFalse((self.root / ".cache").exists())

    def test_events_help_and_saved_dump_rejection_are_available_without_phone(self) -> None:
        """Both loader commands expose event output and offline extraction rejects it."""
        for arguments in (("run",), ("device-data", "prepare")):
            with self.subTest(arguments=arguments):
                result = self.run_cli(*arguments, "--help")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("--events PATH", result.stdout)
        result = self.run_cli(
            "device-data",
            "prepare",
            "example",
            "--from-dump",
            "saved.bin",
            "--events",
            "events.jsonl",
        )
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("cannot be used with --from-dump", result.stderr)
        self.assertFalse((self.root / "events.jsonl").exists())


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Host package tests for the release-qualification payload boundary."""

from __future__ import annotations

import contextlib
import hashlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import alpine_state, commands
from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    canonical_json_bytes,
    publish_current_bundle,
    published_file_records,
)
from fplinux_cli.workspace import WorkspaceSnapshot


class ReleasePackageTests(unittest.TestCase):
    """Exercise real archive creation with host identity inputs isolated."""

    def setUp(self) -> None:
        """Create one complete synthetic bundle and its canonical source documents."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / ".cache"
        self.target = "phone"
        self.snapshot = WorkspaceSnapshot((), "a" * 64)
        self.image_recipe = "b" * 64
        self.target_config = {
            "platform": "demo",
            "runtime": {
                "assets": {"pinmap": "assets/pinmap.bin"},
            },
        }
        self.release_manifest = {
            "image": "image/ramboot.bin",
            "bundle_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/keyboard",
                "runner/run.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
                "apks/demo.apk",
                "assets.lock.toml",
            ],
            "runtime_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/keyboard",
                "runner/run.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
            ],
            "documents": ["release/README.txt"],
        }
        self.platform = {"host": {"tools": [{"name": "keyboard"}]}}
        target_readme = self.root / "targets" / self.target / "release/README.txt"
        target_readme.parent.mkdir(parents=True)
        target_readme.write_text("phone instructions\n", encoding="utf-8")
        self.target_readme = target_readme
        license_file = self.root / "LICENSE"
        license_file.write_text("project license\n", encoding="utf-8")
        musl_notice = self.root / "THIRD_PARTY_LICENSES/musl/COPYRIGHT"
        musl_notice.parent.mkdir(parents=True)
        musl_notice.write_text("musl notice\n", encoding="utf-8")
        self.package_documents = {
            "LICENSE": license_file,
            "licenses/musl/COPYRIGHT": musl_notice,
        }
        signing_key = alpine_state.signing_public_key(self.cache)
        signing_key.parent.mkdir(parents=True)
        signing_key.write_bytes(b"test signing public key\n")
        self.signing_key = hashlib.sha256(signing_key.read_bytes()).hexdigest()
        self.publish_bundle(
            "c" * 64,
            apk=b"application version one\n",
            metadata=b"asset provenance version one\n",
        )

    def publish_bundle(self, generation: str, *, apk: bytes, metadata: bytes) -> None:
        """Publish one valid immutable generation with the requested APK bytes."""
        bundle = self.cache / "out" / self.target / "bundles" / generation
        payloads = {
            "image/ramboot.bin": b"ramboot\n",
            "assets/pinmap.bin": b"pinmap\n",
            "host/keyboard": b"keyboard\n",
            "runner/run.py": b"#!/usr/bin/env python3\n",
            "runner/identity.py": b"identity helper\n",
            "runner/ssh_transport.py": b"ssh helper\n",
            "runner/platform_adapter.py": b"adapter\n",
            "runtime-manifest.json": b"{}\n",
            "apks/demo.apk": apk,
            "assets.lock.toml": metadata,
        }
        for relative, data in payloads.items():
            path = bundle / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if relative in {"host/keyboard", "runner/run.py"} else 0o644)
        manifest = {
            "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
            "container_image_recipe": self.image_recipe,
            "apk_signing_key": self.signing_key,
            "device_identity": "f" * 64,
            "files": published_file_records(bundle),
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": None,
            "target": self.target,
            "workspace_digest": self.snapshot.recipe,
        }
        (bundle / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(self.cache / "out", self.target, bundle)

    def package(self, *, candidate: bool) -> tuple[str, str]:
        """Create an archive and return its archive and qualification digests."""
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            commands.package_target(self.target, candidate=candidate)
        values: dict[str, str] = {}
        for line in stdout.getvalue().splitlines():
            for label in ("Archive SHA256", "Qualification payload SHA256"):
                prefix = f"{label}: "
                if line.startswith(prefix):
                    values[label] = line.removeprefix(prefix)
        if set(values) != {"Archive SHA256", "Qualification payload SHA256"}:
            self.fail("package output omitted an archive or qualification digest")
        return values["Archive SHA256"], values["Qualification payload SHA256"]

    def test_release_runtime_requires_the_shared_identity_helper(self) -> None:
        """Do not package a standalone runner without its validated identity schema."""
        broken = {
            **self.release_manifest,
            "runtime_files": [
                path
                for path in self.release_manifest["runtime_files"]
                if path != "runner/identity.py"
            ],
        }
        with (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_release", return_value=broken),
            mock.patch.object(commands, "load_platform", return_value=self.platform),
            self.assertRaisesRegex(SystemExit, "omit required runtime inputs"),
        ):
            commands.load_release_manifest(self.target, self.target_config)

    def test_apk_bytes_but_not_archive_metadata_change_qualification(self) -> None:
        """Only a changed executable payload requires a new phone qualification."""
        patches = (
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "PACKAGE_DOCUMENTS", self.package_documents),
            mock.patch.object(commands, "load_target", return_value=self.target_config),
            mock.patch.object(commands, "load_release", return_value=self.release_manifest),
            mock.patch.object(commands, "load_platform", return_value=self.platform),
            mock.patch.object(
                commands,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(
                commands,
                "container_image_recipe_digest",
                return_value=self.image_recipe,
            ),
        )
        with contextlib.ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)

            candidate_archive, original = self.package(candidate=True)
            candidates = list((self.cache / "out/candidates").glob("*.zip"))
            self.assertEqual(len(candidates), 1)
            self.assertTrue(candidates[0].name.startswith("FPLinux-phone-candidate-"))
            with mock.patch.object(commands, "verified_runtime_digest", return_value=original):
                release_archive, release_payload = self.package(candidate=False)
            self.assertEqual(release_payload, original)
            self.assertNotEqual(release_archive, candidate_archive)

            self.publish_bundle(
                "3" * 64,
                apk=b"application version one\n",
                metadata=b"asset provenance version two\n",
            )
            metadata_archive, after_metadata = self.package(candidate=True)
            self.assertEqual(after_metadata, original)
            self.assertNotEqual(metadata_archive, candidate_archive)
            with mock.patch.object(commands, "verified_runtime_digest", return_value=original):
                self.package(candidate=False)

            self.publish_bundle(
                "4" * 64,
                apk=b"application version two\n",
                metadata=b"asset provenance version two\n",
            )
            _apk_archive, after_apk = self.package(candidate=True)
            self.assertNotEqual(after_apk, original)
            with (
                mock.patch.object(
                    commands,
                    "verified_runtime_digest",
                    return_value=original,
                ),
                self.assertRaisesRegex(SystemExit, "not hardware-qualified"),
            ):
                commands.package_target(self.target)


if __name__ == "__main__":
    unittest.main()

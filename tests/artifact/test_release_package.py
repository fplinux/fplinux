# SPDX-License-Identifier: GPL-2.0-only
"""Artifact tests for release archives and qualification payloads."""

from __future__ import annotations

import contextlib
import hashlib
import io
import tempfile
import unittest
import zipfile
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


class ReleaseArchiveArtifactTests(unittest.TestCase):
    """Exercise real archive creation with host identity inputs isolated."""

    def setUp(self) -> None:
        """Create one complete synthetic bundle and its canonical source documents."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / ".cache"
        self.target = "nokia-ta1618"
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
            "documents": ["release/README.txt", "features/MICROSD.md"],
        }
        self.platform = {"host": {"tools": [{"name": "keyboard"}]}}
        target_readme = self.root / "targets" / self.target / "release/README.txt"
        target_readme.parent.mkdir(parents=True)
        target_readme.write_text("phone instructions\n", encoding="utf-8")
        self.target_readme = target_readme
        target_feature = self.root / "targets" / self.target / "features/MICROSD.md"
        target_feature.parent.mkdir(parents=True)
        target_feature.write_bytes(b"phone microSD procedures\n")
        self.target_documents = {
            "docs/target/MICROSD.md": target_feature.read_bytes(),
        }
        license_file = self.root / "LICENSE"
        license_file.write_text("project license\n", encoding="utf-8")
        rules_file = self.root / "common/60-fplinux.rules"
        rules_file.parent.mkdir(parents=True)
        rules_file.write_text("SUBSYSTEM==usb\n", encoding="utf-8")
        musl_notice = self.root / "THIRD_PARTY_LICENSES/musl/COPYRIGHT"
        musl_notice.parent.mkdir(parents=True)
        musl_notice.write_text("musl notice\n", encoding="utf-8")
        self.shared_documents = {
            "docs/apps/MICROPYTHONOS.md": b"MicroPythonOS procedures\n",
            "docs/apps/TYRQUAKE.md": b"TyrQuake procedures\n",
            "docs/features/CPU_CLOCK.md": b"CPU clock reporting\n",
            "docs/features/FILE_TRANSFER.md": b"File transfer procedures\n",
            "docs/features/HOST_KEYBOARD.md": b"Host keyboard procedures\n",
            "docs/features/LOCAL_CONSOLE.md": b"Local console procedures\n",
            "docs/features/SSH.md": b"SSH procedures\n",
            "docs/features/USB_NETWORKING.md": b"USB networking procedures\n",
            "docs/guides/STANDALONE.md": b"Standalone archive procedures\n",
        }
        for relative, contents in self.shared_documents.items():
            document = self.root / relative
            document.parent.mkdir(parents=True, exist_ok=True)
            document.write_bytes(contents)
        self.package_documents = {
            "60-fplinux.rules": rules_file,
            "LICENSE": license_file,
            **{relative: self.root / relative for relative in self.shared_documents},
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
            "boot_artifacts": {"required": []},
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

    def package_patches(self) -> tuple[contextlib.AbstractContextManager[object], ...]:
        """Isolate package creation from host identity and repository files."""
        return (
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

    def test_candidate_contains_shared_documents_with_complete_checksums(self) -> None:
        """Publish bundled procedures and cover every archive member by SHA-256."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            self.package(candidate=True)

        archives = list((self.cache / "out/candidates").glob("*.zip"))
        self.assertEqual(len(archives), 1)
        with zipfile.ZipFile(archives[0]) as archive:
            members = archive.namelist()
            roots = {name.partition("/")[0] for name in members}
            self.assertEqual(len(roots), 1)
            root = roots.pop()
            payloads = {name.removeprefix(f"{root}/"): archive.read(name) for name in members}

        expected_documents = {
            "README.txt": b"phone instructions\n",
            **self.target_documents,
            **{
                relative: source.read_bytes()
                for relative, source in self.package_documents.items()
            },
        }
        for relative, expected in expected_documents.items():
            with self.subTest(relative=relative):
                self.assertEqual(payloads[relative], expected)

        checksums = {
            relative: digest
            for line in payloads["SHA256SUMS"].decode("utf-8").splitlines()
            for digest, relative in (line.split("  ", 1),)
        }
        self.assertEqual(set(checksums), set(payloads) - {"SHA256SUMS"})
        for relative, digest in checksums.items():
            with self.subTest(checksum=relative):
                self.assertEqual(digest, hashlib.sha256(payloads[relative]).hexdigest())

    def test_profile_and_microsd_boot_candidates_use_one_generation(self) -> None:
        """Both selectors package the same image with distinct archive context names."""
        profile = "microsd-uboot"
        self.target_config["profile"] = profile
        self.addCleanup(self.target_config.pop, "profile", None)
        profile_root = self.root / "targets" / self.target / "profiles" / profile
        profile_readme = profile_root / "release/README.txt"
        profile_readme.parent.mkdir(parents=True)
        profile_readme.write_bytes(b"profile instructions\n")
        profile_storage = profile_root / "features/MICROSD.md"
        profile_storage.parent.mkdir(parents=True)
        profile_storage.write_bytes(b"profile storage rules\n")
        generation = "3" * 64
        profile_snapshot = WorkspaceSnapshot((), "4" * 64)
        default_bundle = next((self.cache / "out" / self.target / "bundles").iterdir())
        profile_bundle = self.cache.joinpath(
            "out", self.target, "profiles", profile, "bundles", generation
        )
        for source in default_bundle.rglob("*"):
            if not source.is_file() or source.name == BUILD_MANIFEST_NAME:
                continue
            destination = profile_bundle / source.relative_to(default_bundle)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source.read_bytes())
            destination.chmod(source.stat().st_mode & 0o777)
        required = {
            "FPLINUX.img.xz": b"profile whole-card image\n",
        }
        for relative, data in required.items():
            path = profile_bundle / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o644)
        manifest = {
            "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
            "boot_artifacts": {
                "required": list(required),
                "runnable": True,
            },
            "container_image_recipe": self.image_recipe,
            "apk_signing_key": self.signing_key,
            "device_identity": "f" * 64,
            "files": published_file_records(profile_bundle),
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": profile,
            "target": self.target,
            "workspace_digest": profile_snapshot.recipe,
        }
        (profile_bundle / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(
            self.cache / "out",
            self.target,
            profile_bundle,
            profile,
        )

        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            workspace = stack.enter_context(
                mock.patch.object(
                    commands,
                    "target_workspace_snapshot",
                    return_value=profile_snapshot,
                )
            )
            with self.assertRaisesRegex(SystemExit, "only be packaged with --candidate"):
                commands.package_target(
                    self.target,
                    profile=profile,
                    candidate=False,
                )
            commands.package_target(
                self.target,
                profile=profile,
                candidate=True,
            )
            commands.package_target(
                self.target,
                boot="microsd",
                candidate=True,
            )

        self.assertEqual(
            workspace.call_args_list,
            [
                mock.call(self.target, profile),
                mock.call(self.target, profile),
            ],
        )
        archives = list((self.cache / "out/candidates").glob("*.zip"))
        self.assertEqual(len(archives), 2)
        names = {archive.name for archive in archives}
        self.assertTrue(
            any(name.startswith(f"FPLinux-{self.target}-{profile}-candidate-") for name in names)
        )
        self.assertTrue(
            any(name.startswith(f"FPLinux-{self.target}-microsd-candidate-") for name in names)
        )
        for archive_path in archives:
            with zipfile.ZipFile(archive_path) as archive:
                root = archive.namelist()[0].partition("/")[0]
                for relative, data in required.items():
                    self.assertEqual(archive.read(f"{root}/{relative}"), data)
                self.assertEqual(
                    archive.read(f"{root}/README.txt"),
                    profile_readme.read_bytes(),
                )
                self.assertEqual(
                    archive.read(f"{root}/docs/target/MICROSD.md"),
                    profile_storage.read_bytes(),
                )

    def test_apk_bytes_but_not_archive_metadata_change_qualification(self) -> None:
        """Only a changed executable payload requires a new phone qualification."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)

            candidate_archive, original = self.package(candidate=True)
            candidate_files = list((self.cache / "out/candidates").glob("*.zip"))
            self.assertEqual(len(candidate_files), 1)
            self.assertTrue(candidate_files[0].name.startswith("FPLinux-nokia-ta1618-candidate-"))
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

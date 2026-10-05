# SPDX-License-Identifier: GPL-2.0-only
"""Artifact tests for release archives and phone-test payloads."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import posixpath
import re
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock
from urllib.parse import urlsplit

from fplinux_cli import alpine_state, common
from fplinux_cli import image_state as image_states
from fplinux_cli import workspace as workspaces
from fplinux_cli.bundle_state import (
    BUILD_MANIFEST_NAME,
    publish_current_bundle,
)
from fplinux_cli.cli import package as package_commands
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment import images
from fplinux_cli.image_state import ImageState
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.manifests.releases import load_release
from fplinux_cli.workspace import WorkspaceSnapshot

from tests.bundle_support import file_record
from tests.process import run_process

# Load the archived helper by path, as the standalone runner does, then report the
# record visible before the helper's context closes.
ARCHIVED_EVENTS_CONSUMER = """\
import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("fplinux_loader_events", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
output = Path(sys.argv[2])
with module.record_events(
    output, target="nokia-ta1618", profile=None, build_type="release"
) as events:
    events.emit("waiting-for-device")
    sys.stdout.write(output.read_text(encoding="utf-8"))
"""


class ReleaseArchiveArtifactTests(unittest.TestCase):
    """Exercise real archive creation with host identity inputs isolated."""

    def setUp(self) -> None:
        """Create one complete synthetic bundle and its canonical source documents."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.event_helper = (common.ROOT / "common/loader_events.py").read_bytes()
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
                "runner/loader_events.py",
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
                "runner/loader_events.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
            ],
            "documents": ["release/README.txt", "features/MICROSD.md"],
        }
        self.platform = {
            "host": {"runtime_tools": {"keyboard": "keyboard"}, "tools": [{"name": "keyboard"}]}
        }
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
            "docs/apps/SHOWCASE.md": b"FPLinux showcase procedures\n",
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

    def publish_bundle(
        self, generation: str, *, apk: bytes, metadata: bytes, build_type: str = "release"
    ) -> None:
        """Publish one valid immutable generation with the requested APK bytes."""
        bundle = self.cache / "out" / self.target / "builds" / build_type / "bundles" / generation
        payloads = {
            "image/ramboot.bin": b"ramboot\n",
            "assets/pinmap.bin": b"pinmap\n",
            "host/keyboard": b"keyboard\n",
            "runner/run.py": b"#!/usr/bin/env python3\n",
            "runner/loader_events.py": self.event_helper,
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
            "container_image_content": "c" * 64,
            "apk_signing_key": self.signing_key,
            "device_identity": "f" * 64,
            "files": {relative: file_record(bundle / relative) for relative in payloads},
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": None,
            "build_type": build_type,
            "target": self.target,
            "workspace_digest": self.snapshot.recipe,
        }
        (bundle / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(self.cache / "out", self.target, bundle, build_type=build_type)

    def package(self, *, candidate: bool, build_type: str = "release") -> tuple[str, str]:
        """Create an archive and return its archive and phone-test payload digests."""
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            package_commands.package_target(
                self.target, candidate=candidate, build_type=build_type
            )
        values: dict[str, str] = {}
        for line in stdout.getvalue().splitlines():
            for label in ("Archive SHA256", "Phone-test payload SHA256"):
                prefix = f"{label}: "
                if line.startswith(prefix):
                    values[label] = line.removeprefix(prefix)
        if set(values) != {"Archive SHA256", "Phone-test payload SHA256"}:
            self.fail("package output omitted an archive or phone-test payload digest")
        return values["Archive SHA256"], values["Phone-test payload SHA256"]

    def record_phone_tested(self, payload: str) -> None:
        """Record one phone-tested payload in the release verification lock."""
        (self.root / "releases.lock.toml").write_text(
            f'[verified]\n{self.target} = "{payload}"\n', encoding="utf-8"
        )

    def package_patches(self) -> tuple[contextlib.AbstractContextManager[object], ...]:
        """Isolate package creation from host identity and repository files."""
        return (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(package_commands, "PACKAGE_DOCUMENTS", self.package_documents),
            mock.patch.object(targets, "load_target", return_value=self.target_config),
            mock.patch.object(releases, "load_release", return_value=self.release_manifest),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
            mock.patch.object(
                workspaces,
                "target_workspace_snapshot",
                return_value=self.snapshot,
            ),
            mock.patch.object(
                images,
                "container_image_recipe_digest",
                return_value=self.image_recipe,
            ),
            mock.patch.object(
                image_states,
                "load_image_state",
                return_value=ImageState(self.image_recipe, "c" * 64, "c" * 64),
            ),
        )

    def test_debug_package_requires_its_own_build_and_names_the_selected_type(self) -> None:
        """Packaging never substitutes release bytes for an absent debug build."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            with self.assertRaisesRegex(SystemExit, "--build-type debug"):
                self.package(candidate=True, build_type="debug")
            self.assertFalse((self.cache / "out/candidates").exists())
            self.publish_bundle(
                "d" * 64, apk=b"debug apk\n", metadata=b"debug assets\n", build_type="debug"
            )
            self.package(candidate=True, build_type="debug")

        archives = list((self.cache / "out/candidates").glob("*.zip"))
        self.assertEqual(len(archives), 1)
        self.assertTrue(archives[0].name.startswith("FPLinux-nokia-ta1618-debug-candidate-"))
        with zipfile.ZipFile(archives[0]) as archive:
            name = next(
                name for name in archive.namelist() if name.endswith("/build-manifest.json")
            )
            self.assertEqual(json.loads(archive.read(name))["build_type"], "debug")

    def test_candidate_rejects_a_release_input_with_a_mismatched_recorded_size(self) -> None:
        """Even matching bytes and permissions cannot authorize a wrong size record."""
        bundle = self.cache / "out" / self.target / "builds/release/bundles" / ("c" * 64)
        manifest_path = bundle / BUILD_MANIFEST_NAME
        manifest = json.loads(manifest_path.read_bytes())
        manifest["files"]["image/ramboot.bin"]["size"] = 7
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(self.cache / "out", self.target, bundle)

        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            with self.assertRaisesRegex(SystemExit, "release input differs"):
                self.package(candidate=True)

        self.assertFalse((self.cache / "out/candidates").exists())

    def test_candidate_omits_missing_debug_payload_and_includes_required_boot_artifact(
        self,
    ) -> None:
        """Archive validation covers its release subset and additional required boot files."""
        bundle = self.cache / "out" / self.target / "builds/release/bundles" / ("c" * 64)
        boot_image = bundle / "FPLINUX.img.xz"
        boot_image.write_bytes(b"whole-card image\n")
        boot_image.chmod(0o644)
        debug = bundle / "debug/vmlinux"
        debug.parent.mkdir()
        debug.write_bytes(b"host debug output\n")
        debug.chmod(0o644)
        manifest_path = bundle / BUILD_MANIFEST_NAME
        manifest = json.loads(manifest_path.read_bytes())
        manifest["files"]["FPLINUX.img.xz"] = file_record(boot_image)
        manifest["files"]["debug/vmlinux"] = file_record(debug)
        manifest["boot_artifacts"]["required"] = ["FPLINUX.img.xz"]
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(self.cache / "out", self.target, bundle)
        debug.unlink()

        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            self.package(candidate=True)

        archive_path = next((self.cache / "out/candidates").glob("*.zip"))
        with zipfile.ZipFile(archive_path) as archive:
            root = archive.namelist()[0].partition("/")[0]
            self.assertEqual(archive.read(f"{root}/FPLINUX.img.xz"), b"whole-card image\n")
            self.assertNotIn(f"{root}/debug/vmlinux", archive.namelist())
            self.assertEqual(archive.read(f"{root}/image/ramboot.bin"), b"ramboot\n")

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
            "CANDIDATE-NOTICE.txt": (
                b"PHONE-TEST CANDIDATE - DO NOT PUBLISH\n\n"
                b"This archive is for testing on the target phone only.\n"
                b"Candidate packaging does not make it release-ready.\n"
            ),
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

    def test_archived_loader_events_work_without_the_source_checkout(self) -> None:
        """An isolated interpreter writes a flushed record with only the archived helper."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            self.package(candidate=True)
        archive_path = next((self.cache / "out/candidates").glob("*.zip"))
        with zipfile.ZipFile(archive_path) as archive:
            helper_name = next(
                name for name in archive.namelist() if name.endswith("/runner/loader_events.py")
            )
            helper_bytes = archive.read(helper_name)
        extracted = self.root / "standalone"
        extracted.mkdir()
        helper = extracted / "loader_events.py"
        helper.write_bytes(helper_bytes)
        # -I and an empty environment keep the checkout and PYTHONPATH off sys.path.
        result = run_process(
            [
                sys.executable,
                "-I",
                "-c",
                ARCHIVED_EVENTS_CONSUMER,
                str(helper),
                str(extracted / "events.jsonl"),
            ],
            name="archived loader events",
            timeout=30,
            cwd=extracted,
            env={},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(result.stdout)
        record.pop("time")
        self.assertEqual(
            record,
            {
                "event": "waiting-for-device",
                "target": "nokia-ta1618",
                "profile": None,
                "build_type": "release",
            },
        )

    def test_profile_and_microsd_boot_candidates_use_one_generation(self) -> None:
        """Both selectors package the same image with distinct archive context names."""
        profile = "microsd-uboot"
        self.target_config["profile"] = profile
        self.addCleanup(self.target_config.pop, "profile", None)
        generation = "3" * 64
        profile_snapshot = WorkspaceSnapshot((), "4" * 64)
        default_bundle = next(
            (self.cache / "out" / self.target / "builds" / "release" / "bundles").iterdir()
        )
        profile_bundle = self.cache.joinpath(
            "out", self.target, "profiles", profile, "builds", "release", "bundles", generation
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
            "container_image_content": "c" * 64,
            "apk_signing_key": self.signing_key,
            "device_identity": "f" * 64,
            "files": {
                source.relative_to(profile_bundle).as_posix(): file_record(source)
                for source in profile_bundle.rglob("*")
                if source.is_file() and source.name != BUILD_MANIFEST_NAME
            },
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": profile,
            "build_type": "release",
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

        def profile_workspace(
            target: str, selected: str | None = None, build_type: str = "release"
        ) -> WorkspaceSnapshot:
            """Match the published profile bundle only for its own release context."""
            if (target, selected, build_type) == (self.target, profile, "release"):
                return profile_snapshot
            return WorkspaceSnapshot((), "5" * 64)

        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            stack.enter_context(
                mock.patch.object(workspaces, "target_workspace_snapshot", profile_workspace)
            )
            with self.assertRaisesRegex(SystemExit, "only be packaged with --candidate"):
                package_commands.package_target(
                    self.target,
                    profile=profile,
                    candidate=False,
                )
            package_commands.package_target(
                self.target,
                profile=profile,
                candidate=True,
            )
            package_commands.package_target(
                self.target,
                boot="microsd",
                candidate=True,
            )

        archives = list((self.cache / "out/candidates").glob("*.zip"))
        self.assertEqual(len(archives), 2)
        names = {archive.name for archive in archives}
        self.assertTrue(
            any(
                name.startswith(f"FPLinux-{self.target}-{profile}-release-candidate-")
                for name in names
            )
        )
        self.assertTrue(
            any(
                name.startswith(f"FPLinux-{self.target}-microsd-release-candidate-")
                for name in names
            )
        )
        for archive_path in archives:
            with zipfile.ZipFile(archive_path) as archive:
                root = archive.namelist()[0].partition("/")[0]
                for relative, data in required.items():
                    self.assertEqual(archive.read(f"{root}/{relative}"), data)
                self.assertEqual(
                    archive.read(f"{root}/README.txt"),
                    b"phone instructions\n",
                )
                self.assertEqual(
                    archive.read(f"{root}/docs/target/MICROSD.md"),
                    b"phone microSD procedures\n",
                )

    def test_relocated_feature_links_reach_bundled_guides(self) -> None:
        """Target links keep their destination and fragments after archive relocation."""
        source = self.root / "targets" / self.target / "features/MICROSD.md"
        source.write_text(
            "[Transfer](../../../docs/features/FILE_TRANSFER.md#upload)\n"
            "[Card](MICROSD.md) [Section](#details)\n"
            "[External](https://example.org/docs/)\n",
            encoding="utf-8",
        )
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            self.package(candidate=True)

        archive_path = next((self.cache / "out/candidates").glob("*.zip"))
        with zipfile.ZipFile(archive_path) as archive:
            root = archive.namelist()[0].partition("/")[0]
            self.assertEqual(
                archive.read(f"{root}/docs/target/MICROSD.md"),
                b"[Transfer](../features/FILE_TRANSFER.md#upload)\n"
                b"[Card](MICROSD.md) [Section](#details)\n"
                b"[External](https://example.org/docs/)\n",
            )
            self.assertEqual(
                archive.read(f"{root}/docs/features/FILE_TRANSFER.md"),
                b"File transfer procedures\n",
            )

    def test_bundled_document_links_resolve_without_a_source_checkout(self) -> None:
        """Actual release pages only link to local files included in the archive."""
        source_root = Path(__file__).resolve().parents[2]
        self.release_manifest["documents"] = load_release(self.target)["documents"]
        for relative in self.release_manifest["documents"]:
            source = source_root / "targets" / self.target / relative
            destination = self.root / "targets" / self.target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        self.package_documents = {}
        for relative, source in package_commands.PACKAGE_DOCUMENTS.items():
            destination = self.root / source.relative_to(source_root)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            self.package_documents[relative] = destination

        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            self.package(candidate=True)

        archive_path = next((self.cache / "out/candidates").glob("*.zip"))
        with zipfile.ZipFile(archive_path) as archive:
            members = set(archive.namelist())
            for name in sorted(members):
                if not name.endswith(".md"):
                    continue
                document = archive.read(name).decode("utf-8")
                for link in re.findall(r"\]\(([^\s)]+)\)", document):
                    url = urlsplit(link)
                    if url.scheme or url.netloc or not url.path:
                        continue
                    linked_member = posixpath.normpath(
                        posixpath.join(posixpath.dirname(name), url.path)
                    )
                    with self.subTest(document=name, link=link):
                        self.assertIn(linked_member, members)

    def test_apk_bytes_but_not_archive_metadata_change_phone_test_payload(self) -> None:
        """Only a changed executable payload requires another complete phone test."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)

            candidate_archive, original = self.package(candidate=True)
            candidate_files = list((self.cache / "out/candidates").glob("*.zip"))
            self.assertEqual(len(candidate_files), 1)
            self.assertTrue(
                candidate_files[0].name.startswith("FPLinux-nokia-ta1618-release-candidate-")
            )
            self.record_phone_tested(original)
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
            self.package(candidate=False)

            self.publish_bundle(
                "4" * 64,
                apk=b"application version two\n",
                metadata=b"asset provenance version two\n",
            )
            _apk_archive, after_apk = self.package(candidate=True)
            self.assertNotEqual(after_apk, original)
            with self.assertRaisesRegex(SystemExit, "not phone-tested"):
                package_commands.package_target(self.target)


if __name__ == "__main__":
    unittest.main()

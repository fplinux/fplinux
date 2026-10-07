# SPDX-License-Identifier: GPL-2.0-only
"""Artifact tests for release archives and phone-test payloads."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import zipfile
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import signing as alpine_state
from fplinux_cli.artifacts.bundles import BUILD_MANIFEST_NAME, publish_current_bundle
from fplinux_cli.cli import package as package_commands
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment import image_state as image_states
from fplinux_cli.environment import images
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.workspace import build_inputs as workspaces
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.bundle_support import file_record
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

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


class ReleaseArchiveArtifactTests:
    """Exercise real archive creation with host identity inputs isolated."""

    @pytest.fixture(autouse=True)
    def _prepare_inputs(self, tmp_path: Path) -> None:
        """Create a synthetic bundle, legal notices and checkout-only documentation."""
        self.root = tmp_path
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
            "documents": ["release/README.txt"],
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
        license_file = self.root / "LICENSE"
        license_file.write_text("project license\n", encoding="utf-8")
        rules_file = self.root / "common/60-fplinux.rules"
        rules_file.parent.mkdir(parents=True)
        rules_file.write_text("SUBSYSTEM==usb\n", encoding="utf-8")
        musl_notice = self.root / "THIRD_PARTY_LICENSES/musl/COPYRIGHT"
        musl_notice.parent.mkdir(parents=True)
        musl_notice.write_text("musl notice\n", encoding="utf-8")
        checkout_documents = {
            "docs/apps/SHOWCASE.md": b"FPLinux showcase procedures\n",
            "docs/features/FILE_TRANSFER.md": b"File transfer procedures\n",
            "docs/guides/STANDALONE.md": b"Standalone archive procedures\n",
        }
        for relative, contents in checkout_documents.items():
            document = self.root / relative
            document.parent.mkdir(parents=True, exist_ok=True)
            document.write_bytes(contents)
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
            pytest.fail("package output omitted an archive or phone-test payload digest")
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
            with pytest.raises(SystemExit, match="--build-type debug"):
                self.package(candidate=True, build_type="debug")
            assert not ((self.cache / "out/candidates").exists())
            self.publish_bundle(
                "d" * 64, apk=b"debug apk\n", metadata=b"debug assets\n", build_type="debug"
            )
            self.package(candidate=True, build_type="debug")

        archives = list((self.cache / "out/candidates").glob("*.zip"))
        assert (len(archives)) == (1)
        assert archives[0].name.startswith("FPLinux-nokia-ta1618-debug-candidate-")
        with zipfile.ZipFile(archives[0]) as archive:
            name = next(
                name for name in archive.namelist() if name.endswith("/build-manifest.json")
            )
            assert (json.loads(archive.read(name))["build_type"]) == ("debug")

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
            with pytest.raises(SystemExit, match="release input differs"):
                self.package(candidate=True)

        assert not ((self.cache / "out/candidates").exists())

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
            assert (archive.read(f"{root}/FPLINUX.img.xz")) == (b"whole-card image\n")
            assert (f"{root}/debug/vmlinux") not in (archive.namelist())
            assert (archive.read(f"{root}/image/ramboot.bin")) == (b"ramboot\n")

    def test_candidate_keeps_runtime_and_notices_without_checkout_documentation(self) -> None:
        """Runtime files and legal notices retain bytes, modes and complete checksums."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)
            self.package(candidate=True)

        archives = list((self.cache / "out/candidates").glob("*.zip"))
        assert (len(archives)) == (1)
        with zipfile.ZipFile(archives[0]) as archive:
            members = archive.namelist()
            roots = {name.partition("/")[0] for name in members}
            assert (len(roots)) == (1)
            root = roots.pop()
            payloads = {name.removeprefix(f"{root}/"): archive.read(name) for name in members}
            modes = {
                name.removeprefix(f"{root}/"): archive.getinfo(name).external_attr >> 16
                for name in members
            }

        expected_files = {
            "CANDIDATE-NOTICE.txt": (
                b"PHONE-TEST CANDIDATE - DO NOT PUBLISH\n\n"
                b"This archive is for testing on the target phone only.\n"
                b"Candidate packaging does not make it release-ready.\n"
            ),
            "README.txt": b"phone instructions\n",
            "60-fplinux.rules": b"SUBSYSTEM==usb\n",
            "LICENSE": b"project license\n",
            "licenses/musl/COPYRIGHT": b"musl notice\n",
            "image/ramboot.bin": b"ramboot\n",
            "assets/pinmap.bin": b"pinmap\n",
            "host/keyboard": b"keyboard\n",
            "runner/run.py": b"#!/usr/bin/env python3\n",
            "runner/identity.py": b"identity helper\n",
            "runner/ssh_transport.py": b"ssh helper\n",
            "runner/platform_adapter.py": b"adapter\n",
            "runtime-manifest.json": b"{}\n",
            "apks/demo.apk": b"application version one\n",
            "assets.lock.toml": b"asset provenance version one\n",
        }
        for relative, expected in expected_files.items():
            assert (payloads[relative]) == (expected)
        assert payloads["runner/loader_events.py"] == self.event_helper
        assert set(payloads) == {
            *expected_files,
            "runner/loader_events.py",
            "build-manifest.json",
            "SHA256SUMS",
        }
        for relative, mode in modes.items():
            expected_mode = (
                0o100755 if relative in {"host/keyboard", "runner/run.py"} else 0o100644
            )
            assert mode == expected_mode

        checksums = {
            relative: digest
            for line in payloads["SHA256SUMS"].decode("utf-8").splitlines()
            for digest, relative in (line.split("  ", 1),)
        }
        assert (set(checksums)) == (set(payloads) - {"SHA256SUMS"})
        for relative, digest in checksums.items():
            assert (digest) == (hashlib.sha256(payloads[relative]).hexdigest())

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
        assert (result.returncode) == (0), result.stderr
        record = json.loads(result.stdout)
        record.pop("time")
        assert (record) == (
            {
                "event": "waiting-for-device",
                "target": "nokia-ta1618",
                "profile": None,
                "build_type": "release",
            }
        )

    def test_profile_and_microsd_boot_candidates_use_one_generation(self) -> None:
        """Both selectors package the same image with distinct archive context names."""
        profile = "microsd-uboot"
        self.target_config["profile"] = profile
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
            with pytest.raises(SystemExit, match="only be packaged with --candidate"):
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
        assert (len(archives)) == (2)
        names = {archive.name for archive in archives}
        assert any(
            name.startswith(f"FPLinux-{self.target}-{profile}-release-candidate-")
            for name in names
        )
        assert any(
            name.startswith(f"FPLinux-{self.target}-microsd-release-candidate-") for name in names
        )
        for archive_path in archives:
            with zipfile.ZipFile(archive_path) as archive:
                root = archive.namelist()[0].partition("/")[0]
                for relative, data in required.items():
                    assert (archive.read(f"{root}/{relative}")) == (data)
                assert (archive.read(f"{root}/README.txt")) == (b"phone instructions\n")

    def test_apk_bytes_but_not_archive_metadata_change_phone_test_payload(self) -> None:
        """Only a changed executable payload requires another complete phone test."""
        with contextlib.ExitStack() as stack:
            for patch in self.package_patches():
                stack.enter_context(patch)

            candidate_archive, original = self.package(candidate=True)
            candidate_files = list((self.cache / "out/candidates").glob("*.zip"))
            assert (len(candidate_files)) == (1)
            assert candidate_files[0].name.startswith("FPLinux-nokia-ta1618-release-candidate-")
            self.record_phone_tested(original)
            release_archive, release_payload = self.package(candidate=False)
            assert (release_payload) == (original)
            assert (release_archive) != (candidate_archive)

            self.publish_bundle(
                "3" * 64,
                apk=b"application version one\n",
                metadata=b"asset provenance version two\n",
            )
            metadata_archive, after_metadata = self.package(candidate=True)
            assert (after_metadata) == (original)
            assert (metadata_archive) != (candidate_archive)
            self.package(candidate=False)

            self.publish_bundle(
                "4" * 64,
                apk=b"application version two\n",
                metadata=b"asset provenance version two\n",
            )
            _apk_archive, after_apk = self.package(candidate=True)
            assert (after_apk) != (original)
            with pytest.raises(SystemExit, match="not phone-tested"):
                package_commands.package_target(self.target)

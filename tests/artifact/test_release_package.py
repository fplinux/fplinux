# SPDX-License-Identifier: GPL-2.0-only
"""Artifact tests for release archives and phone-test payloads."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
import tarfile
import zipfile
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import registration
from fplinux_cli.alpine import signing as alpine_state
from fplinux_cli.artifacts.bundles import BUILD_MANIFEST_NAME, publish_current_bundle
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment import definition
from fplinux_cli.environment import state as environment_states
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.packaging import archive as package_commands
from fplinux_cli.workspace import build_inputs as workspaces
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests import ROOT
from tests.bundle_support import file_record
from tests.process import run_process
from tests.small.cli.package_docs_fixtures import write_offline_export

if TYPE_CHECKING:
    from pathlib import Path
    from typing import Any


class ReleaseArchiveInputs:
    """Own one synthetic bundle and the checkout inputs used to package it."""

    def __init__(self, root: Path) -> None:
        """Create a synthetic bundle, notices, offline export and checkout-only documents."""
        self.root = root
        self.documentation = write_offline_export(self.root)
        self.event_helper = (common.ROOT / "scripts/fplinux_cli/runtime/events.py").read_bytes()
        self.cache = self.root / ".cache"
        self.target = "nokia-ta1618"
        self.snapshot = WorkspaceSnapshot((), "a" * 64)
        self.environment_recipe = "b" * 64
        self.target_config: dict[str, Any] = {
            "platform": "demo",
            "identity": {"brand": "ACME", "product": "Demo", "hardware_codes": ["D-1"]},
            "adapter": {"boot_instructions": "Hold the left soft key while connecting USB."},
            "linux": {"root": {"kind": "initramfs"}},
            "rootfs": {"base_packages": [], "packages": []},
            "bundle": {"packages": []},
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
                "runtime-manifest.json",
                "apks/demo.apk",
                "assets.lock.toml",
            ],
            "runtime_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/keyboard",
                "runtime-manifest.json",
            ],
            "documents": ["release/EXTRA.txt"],
        }
        self.platform = {
            "rootfs": {"packages": ["fplinux-font-terminus-6x12"]},
            "bundle": {"packages": ["demo"]},
            "host": {"runtime_tools": {"keyboard": "keyboard"}, "tools": [{"name": "keyboard"}]},
        }
        for package in (*registration.COMMON_PACKAGES, "demo"):
            apkbuild = self.root / "alpine/aports" / package / "APKBUILD"
            apkbuild.parent.mkdir(parents=True)
            apkbuild.write_text(f"pkgname={package}\n", encoding="utf-8")
        target_readme = self.root / "targets" / self.target / "release/EXTRA.txt"
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
        musl_notice = self.root / "third-party-licenses/musl/COPYRIGHT"
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
        self,
        generation: str,
        *,
        apk: bytes,
        metadata: bytes,
        build_type: str = "release",
        runtime_sources: dict[str, bytes] | None = None,
    ) -> None:
        """Publish one valid immutable generation with the requested APK bytes."""
        bundle = self.cache / "out" / self.target / "builds" / build_type / "bundles" / generation
        payloads = {
            "60-fplinux.rules": b"SUBSYSTEM==usb\n",
            "image/ramboot.bin": b"ramboot\n",
            "device-data.cpio": b"synthetic phone capsule\n",
            "assets/pinmap.bin": b"pinmap\n",
            "host/keyboard": b"keyboard\n",
            "fplinux.py": b"#!/usr/bin/env python3\n",
            "scripts/fplinux_cli/runtime/events.py": self.event_helper,
            "scripts/fplinux_cli/manifests/identity.py": b"identity helper\n",
            "scripts/fplinux_cli/runtime/ssh_transport.py": b"ssh helper\n",
            "platforms/demo/host/adapter.py": b"adapter\n",
            "apks/demo.apk": apk,
            "assets.lock.toml": metadata,
            **(runtime_sources or {}),
        }
        runtime = {
            "nand": None,
            "device_data": None,
            "host_tools": {"keyboard": "host/keyboard"},
            "sha256": {
                relative: hashlib.sha256(contents).hexdigest()
                for relative, contents in payloads.items()
                if not relative.startswith("apks/")
                and relative not in {"assets.lock.toml", "device-data.cpio"}
            },
        }
        payloads["runtime-manifest.json"] = canonical_json_bytes(runtime)
        for relative, data in payloads.items():
            path = bundle / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if relative in {"host/keyboard", "fplinux.py"} else 0o644)
        manifest = {
            "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
            "kernel_artifacts": {},
            "boot_artifacts": {"required": []},
            "environment_recipe": self.environment_recipe,
            "environment_content": "c" * 64,
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

    def prepare_loader_license_sources(self) -> None:
        """Supply exact admitted source archives without replacing notice collection."""
        digests = {}
        for name, members in {
            "board": {
                "board-release/Licenses/README": b"original board license explanation\n",
                "board-release/Licenses/gpl-2.0.txt": b"original board GPL terms\n",
            },
            "monitor": {"monitor-release/license.md": b"original secure monitor license\n"},
        }.items():
            path = self.cache / "downloads/loaders" / f"{name}.tar.gz"
            path.parent.mkdir(parents=True, exist_ok=True)
            with tarfile.open(path, "w:gz") as archive:
                for member_name, contents in members.items():
                    member = tarfile.TarInfo(member_name)
                    member.size = len(contents)
                    archive.addfile(member, io.BytesIO(contents))
            digests[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        template = (
            ROOT / "tests/artifact/fixtures/release-package/loader-sources.lock.toml.in"
        ).read_text(encoding="utf-8")
        (self.root / "sources.lock.toml").write_text(
            template.format(board_sha256=digests["board"], monitor_sha256=digests["monitor"]),
            encoding="utf-8",
        )
        self.target_config["loader"] = {
            "build": {"fdl1_source": "first", "fdl2_source": "board", "sml_source": "monitor"}
        }

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
                definition,
                "environment_recipe_digest",
                return_value=self.environment_recipe,
            ),
            mock.patch.object(
                environment_states,
                "load_environment_state",
                return_value=EnvironmentState(self.environment_recipe, "c" * 64, "c" * 64),
            ),
        )


@pytest.fixture
def release_inputs(tmp_path: Path) -> ReleaseArchiveInputs:
    """Keep the bundle and mutable release settings separate for every case."""
    return ReleaseArchiveInputs(tmp_path)


class ReleaseArchiveArtifactTests:
    """Exercise real archive creation with host identity inputs isolated."""

    @pytest.mark.parametrize("platform", ["ums9117", "sc9820e"])
    def test_ordinary_archive_accepts_current_normal_or_clean_build_but_rejects_changed_sources(
        self, release_inputs: ReleaseArchiveInputs, platform: str
    ) -> None:
        """Real identity admission accepts either build policy for the unchanged sources."""
        inputs = release_inputs
        inputs.target_config["platform"] = platform
        if platform == "sc9820e":
            inputs.prepare_loader_license_sources()
        sources_changed = False

        def package_snapshot(
            *_arguments: object, include_device_data: bool = True, **_options: object
        ) -> WorkspaceSnapshot:
            """Replace capture while keeping normal, clean and changed sources distinct."""
            if sources_changed:
                return WorkspaceSnapshot((), ("7" if include_device_data else "8") * 64)
            return WorkspaceSnapshot((), ("a" if include_device_data else "9") * 64)

        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            stack.enter_context(
                mock.patch.object(workspaces, "target_workspace_snapshot", package_snapshot)
            )
            inputs.package(candidate=True)
            inputs.snapshot = WorkspaceSnapshot((), "9" * 64)
            inputs.publish_bundle(
                "d" * 64,
                apk=b"application version one\n",
                metadata=b"asset provenance version one\n",
            )
            inputs.package(candidate=True)
            sources_changed = True
            with pytest.raises(SystemExit, match="build output is stale"):
                inputs.package(candidate=True)

        workspace_digests: set[str] = set()
        archives = list((inputs.cache / "out/candidates").glob("*.zip"))
        assert len(archives) == 2
        for archive_path in archives:
            with zipfile.ZipFile(archive_path) as archive:
                root = archive.namelist()[0].partition("/")[0]
                manifest = json.loads(archive.read(f"{root}/build-manifest.json"))
                workspace_digests.add(manifest["workspace_digest"])
                assert archive.read(f"{root}/image/ramboot.bin") == b"ramboot\n"
        assert workspace_digests == {"a" * 64, "9" * 64}

    @pytest.mark.parametrize("platform", ["ums9117", "sc9820e"])
    def test_ordinary_archive_rejects_an_owner_build_after_personal_device_data_changes(
        self, release_inputs: ReleaseArchiveInputs, platform: str
    ) -> None:
        """A clean snapshot cannot admit a build containing different personal inputs."""
        inputs = release_inputs
        inputs.target_config["platform"] = platform
        if platform == "sc9820e":
            inputs.prepare_loader_license_sources()

        def package_snapshot(
            *_arguments: object, include_device_data: bool = True, **_options: object
        ) -> WorkspaceSnapshot:
            """Retain the source-only recipe while changing the personal-data recipe."""
            return WorkspaceSnapshot((), ("6" if include_device_data else "9") * 64)

        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            stack.enter_context(
                mock.patch.object(workspaces, "target_workspace_snapshot", package_snapshot)
            )
            with pytest.raises(SystemExit, match="build output is stale"):
                inputs.package(candidate=True)
        assert not (inputs.cache / "out/candidates").exists()

    def test_sc9820e_linux_archive_delivers_built_loaders_without_private_inputs(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """An ordinary archive retains every admitted stage and needs no loader import."""
        release_inputs.target_config["platform"] = "sc9820e"
        release_inputs.prepare_loader_license_sources()
        bundle = (
            release_inputs.cache
            / "out"
            / release_inputs.target
            / "builds/release/bundles"
            / ("c" * 64)
        )
        stages = {
            "assets/fdl1.bin": b"built first stage\n",
            "assets/fdl2.bin": b"built board initializer\n",
            "assets/sml.bin": b"built secure monitor\n",
        }
        release_inputs.target_config["runtime"]["assets"] = {
            "fdl1": "assets/fdl1.bin",
            "fdl2": "assets/fdl2.bin",
            "sml": "assets/sml.bin",
        }
        for relative, contents in stages.items():
            stage = bundle / relative
            stage.write_bytes(contents)
            stage.chmod(0o644)
        for field in ("bundle_files", "runtime_files"):
            release_inputs.release_manifest[field] = [
                *release_inputs.release_manifest[field],
                *stages,
            ]
        manifest_path = bundle / BUILD_MANIFEST_NAME
        manifest = json.loads(manifest_path.read_bytes())
        manifest["files"].update({relative: file_record(bundle / relative) for relative in stages})
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(release_inputs.cache / "out", release_inputs.target, bundle)
        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            release_inputs.package(candidate=True)

        archive_path = next((release_inputs.cache / "out/candidates").glob("*.zip"))
        with zipfile.ZipFile(archive_path) as archive:
            root = archive.namelist()[0].partition("/")[0]
            payloads = {
                name.removeprefix(f"{root}/"): archive.read(name) for name in archive.namelist()
            }
            assert {relative: payloads[relative] for relative in stages} == {
                "assets/fdl1.bin": b"built first stage\n",
                "assets/fdl2.bin": b"built board initializer\n",
                "assets/sml.bin": b"built secure monitor\n",
            }
            assert payloads["device-data.cpio"] == b"synthetic phone capsule\n"
            assert (
                payloads["licenses/loaders/fdl2/README"] == b"original board license explanation\n"
            )
            assert payloads["licenses/loaders/fdl2/gpl-2.0.txt"] == b"original board GPL terms\n"
            assert (
                payloads["licenses/loaders/sml/license.md"] == b"original secure monitor license\n"
            )
            assert payloads["host/keyboard"] == b"keyboard\n"
            assert payloads["fplinux.py"] == b"#!/usr/bin/env python3\n"
            assert "python3.14 -I fplinux.py run\n" in payloads["README.txt"].decode()
            assert "--loader-inputs" not in payloads["README.txt"].decode()
            assert "python3.14 -I fplinux.py run --reconnect" in payloads["README.txt"].decode()
            assert "Run.py" not in payloads
            assert not any(name.startswith(".cache/") for name in payloads)
            assert all(
                archive.getinfo(f"{root}/{relative}").external_attr >> 16 == 0o100644
                for relative in stages
            )
            assert archive.getinfo(f"{root}/fplinux.py").external_attr >> 16 == 0o100755

    def test_package_carries_the_bundle_runtime_and_qualifies_its_bytes(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Runtime source bytes come from the selected bundle and affect phone qualification."""
        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            _archive_digest, first_digest = release_inputs.package(candidate=True)
            release_inputs.publish_bundle(
                "d" * 64,
                apk=b"application version one\n",
                metadata=b"asset provenance version one\n",
                runtime_sources={"scripts/fplinux_cli/runtime/workflow.py": b"frozen workflow\n"},
            )
            _archive_digest, second_digest = release_inputs.package(candidate=True)

        assert first_digest != second_digest
        payloads = []
        for archive_path in (release_inputs.cache / "out/candidates").glob("*.zip"):
            with zipfile.ZipFile(archive_path) as archive:
                payloads.append(
                    {name.split("/", 1)[1]: archive.read(name) for name in archive.namelist()}
                )
        runtime = next(
            payload for payload in payloads if "scripts/fplinux_cli/runtime/workflow.py" in payload
        )
        assert runtime["scripts/fplinux_cli/runtime/workflow.py"] == b"frozen workflow\n"

    def test_debug_package_requires_its_own_build_and_names_the_selected_type(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Packaging never substitutes release bytes for an absent debug build."""
        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            with pytest.raises(SystemExit, match="--build-type debug"):
                release_inputs.package(candidate=True, build_type="debug")
            assert not ((release_inputs.cache / "out/candidates").exists())
            release_inputs.publish_bundle(
                "d" * 64, apk=b"debug apk\n", metadata=b"debug assets\n", build_type="debug"
            )
            release_inputs.package(candidate=True, build_type="debug")

        archives = list((release_inputs.cache / "out/candidates").glob("*.zip"))
        assert (len(archives)) == (1)
        assert archives[0].name.startswith("FPLinux-nokia-ta1618-debug-candidate-")
        with zipfile.ZipFile(archives[0]) as archive:
            name = next(
                name for name in archive.namelist() if name.endswith("/build-manifest.json")
            )
            assert (json.loads(archive.read(name))["build_type"]) == ("debug")

    def test_candidate_rejects_a_release_input_with_a_mismatched_recorded_size(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Even matching bytes and permissions cannot authorize a wrong size record."""
        bundle = (
            release_inputs.cache
            / "out"
            / release_inputs.target
            / "builds/release/bundles"
            / ("c" * 64)
        )
        manifest_path = bundle / BUILD_MANIFEST_NAME
        manifest = json.loads(manifest_path.read_bytes())
        manifest["files"]["image/ramboot.bin"]["size"] = 7
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(release_inputs.cache / "out", release_inputs.target, bundle)

        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            with pytest.raises(SystemExit, match="release input differs"):
                release_inputs.package(candidate=True)

        assert not ((release_inputs.cache / "out/candidates").exists())

    def test_candidate_omits_missing_debug_payload_and_includes_required_boot_artifact(
        self,
        release_inputs: ReleaseArchiveInputs,
    ) -> None:
        """Archive validation covers its release subset and additional required boot files."""
        bundle = (
            release_inputs.cache
            / "out"
            / release_inputs.target
            / "builds/release/bundles"
            / ("c" * 64)
        )
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
        publish_current_bundle(release_inputs.cache / "out", release_inputs.target, bundle)
        debug.unlink()

        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            release_inputs.package(candidate=True)

        archive_path = next((release_inputs.cache / "out/candidates").glob("*.zip"))
        with zipfile.ZipFile(archive_path) as archive:
            root = archive.namelist()[0].partition("/")[0]
            assert (archive.read(f"{root}/FPLINUX.img.xz")) == (b"whole-card image\n")
            assert (f"{root}/debug/vmlinux") not in (archive.namelist())
            assert (archive.read(f"{root}/image/ramboot.bin")) == (b"ramboot\n")

    def test_candidate_keeps_runtime_and_notices_without_checkout_documentation(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Generated docs join runtime and notices; checkout-only documents stay outside."""
        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            release_inputs.package(candidate=True)

        archives = list((release_inputs.cache / "out/candidates").glob("*.zip"))
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
                ROOT / "tests/fixtures/release-package/candidate-notice.txt"
            ).read_bytes(),
            "EXTRA.txt": b"phone instructions\n",
            "60-fplinux.rules": b"SUBSYSTEM==usb\n",
            "LICENSE": b"project license\n",
            "licenses/musl/COPYRIGHT": b"musl notice\n",
            "image/ramboot.bin": b"ramboot\n",
            "device-data.cpio": b"synthetic phone capsule\n",
            "assets/pinmap.bin": b"pinmap\n",
            "host/keyboard": b"keyboard\n",
            "fplinux.py": b"#!/usr/bin/env python3\n",
            "scripts/fplinux_cli/manifests/identity.py": b"identity helper\n",
            "scripts/fplinux_cli/runtime/ssh_transport.py": b"ssh helper\n",
            "platforms/demo/host/adapter.py": b"adapter\n",
            "apks/demo.apk": b"application version one\n",
            "assets.lock.toml": b"asset provenance version one\n",
            **release_inputs.documentation,
        }
        for relative, expected in expected_files.items():
            assert (payloads[relative]) == (expected)
        assert payloads["README.txt"].startswith(b"FPLinux for ACME Demo (D-1)\n")
        assert b"unmount all data-card filesystems" in payloads["README.txt"]
        assert payloads["scripts/fplinux_cli/runtime/events.py"] == release_inputs.event_helper
        runtime = json.loads(payloads["runtime-manifest.json"])
        assert all(
            hashlib.sha256(payloads[name]).hexdigest() == digest
            for name, digest in runtime["sha256"].items()
        )
        assert set(payloads) == {
            *expected_files,
            "scripts/fplinux_cli/runtime/events.py",
            "runtime-manifest.json",
            "build-manifest.json",
            "SHA256SUMS",
            "README.txt",
        }
        for relative, mode in modes.items():
            expected_mode = 0o100755 if relative in {"host/keyboard", "fplinux.py"} else 0o100644
            assert mode == expected_mode

        checksums = {
            relative: digest
            for line in payloads["SHA256SUMS"].decode("utf-8").splitlines()
            for digest, relative in (line.split("  ", 1),)
        }
        assert (set(checksums)) == (set(payloads) - {"SHA256SUMS"})
        for relative, digest in checksums.items():
            assert (digest) == (hashlib.sha256(payloads[relative]).hexdigest())

    def test_archived_loader_events_work_without_the_source_checkout(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """An isolated interpreter imports the archived package and writes a flushed record."""
        release_inputs.publish_bundle(
            "d" * 64,
            apk=b"application version one\n",
            metadata=b"asset provenance version one\n",
            runtime_sources={
                relative: (ROOT / relative).read_bytes()
                for relative in (
                    "scripts/fplinux_cli/__init__.py",
                    "scripts/fplinux_cli/common.py",
                    "scripts/fplinux_cli/runtime/__init__.py",
                )
            },
        )
        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            release_inputs.package(candidate=True)
        archive_path = next((release_inputs.cache / "out/candidates").glob("*.zip"))
        extracted = release_inputs.root / "standalone"
        extracted.mkdir()
        with zipfile.ZipFile(archive_path) as archive:
            archive.extractall(extracted)  # noqa: S202 -- test-owned ZIP from controlled inputs.
        archive_root = next(extracted.iterdir())
        # -I and an empty environment keep the checkout and PYTHONPATH off sys.path.
        result = run_process(
            [
                sys.executable,
                "-I",
                str(ROOT / "tests/artifact/fixtures/release-package/archived_events_consumer.py"),
                str(archive_root),
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

    def test_profile_and_microsd_boot_candidates_use_one_generation(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Both selectors package the same image with distinct archive context names."""
        profile = "microsd-uboot"
        release_inputs.target_config["profile"] = profile
        generation = "3" * 64
        profile_snapshot = WorkspaceSnapshot((), "4" * 64)
        default_bundle = next(
            (
                release_inputs.cache
                / "out"
                / release_inputs.target
                / "builds"
                / "release"
                / "bundles"
            ).iterdir()
        )
        profile_bundle = release_inputs.cache.joinpath(
            "out",
            release_inputs.target,
            "profiles",
            profile,
            "builds",
            "release",
            "bundles",
            generation,
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
            "kernel_artifacts": {},
            "boot_artifacts": {
                "required": list(required),
                "runnable": True,
            },
            "environment_recipe": release_inputs.environment_recipe,
            "environment_content": "c" * 64,
            "apk_signing_key": release_inputs.signing_key,
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
            "target": release_inputs.target,
            "workspace_digest": profile_snapshot.recipe,
        }
        (profile_bundle / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(
            release_inputs.cache / "out",
            release_inputs.target,
            profile_bundle,
            profile,
        )

        def profile_workspace(
            target: str, selected: str | None = None, build_type: str = "release"
        ) -> WorkspaceSnapshot:
            """Match the published profile bundle only for its own release context."""
            if (target, selected, build_type) == (release_inputs.target, profile, "release"):
                return profile_snapshot
            return WorkspaceSnapshot((), "5" * 64)

        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)
            stack.enter_context(
                mock.patch.object(workspaces, "target_workspace_snapshot", profile_workspace)
            )
            with pytest.raises(SystemExit, match="only be packaged with --candidate"):
                package_commands.package_target(
                    release_inputs.target,
                    profile=profile,
                    candidate=False,
                )
            package_commands.package_target(
                release_inputs.target,
                profile=profile,
                candidate=True,
            )
            package_commands.package_target(
                release_inputs.target,
                boot="microsd",
                candidate=True,
            )

        archives = list((release_inputs.cache / "out/candidates").glob("*.zip"))
        assert (len(archives)) == (2)
        names = {archive.name for archive in archives}
        assert any(
            name.startswith(f"FPLinux-{release_inputs.target}-{profile}-release-candidate-")
            for name in names
        )
        assert any(
            name.startswith(f"FPLinux-{release_inputs.target}-microsd-release-candidate-")
            for name in names
        )
        for archive_path in archives:
            with zipfile.ZipFile(archive_path) as archive:
                root = archive.namelist()[0].partition("/")[0]
                for relative, data in required.items():
                    assert (archive.read(f"{root}/{relative}")) == (data)
                assert (archive.read(f"{root}/EXTRA.txt")) == (b"phone instructions\n")

    def test_apk_bytes_but_not_archive_metadata_change_phone_test_payload(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Only a changed executable payload requires another complete phone test."""
        with contextlib.ExitStack() as stack:
            for patch in release_inputs.package_patches():
                stack.enter_context(patch)

            candidate_archive, original = release_inputs.package(candidate=True)
            candidate_files = list((release_inputs.cache / "out/candidates").glob("*.zip"))
            assert (len(candidate_files)) == (1)
            assert candidate_files[0].name.startswith("FPLinux-nokia-ta1618-release-candidate-")
            release_inputs.record_phone_tested(original)
            release_archive, release_payload = release_inputs.package(candidate=False)
            assert (release_payload) == (original)
            assert (release_archive) != (candidate_archive)

            release_inputs.publish_bundle(
                "3" * 64,
                apk=b"application version one\n",
                metadata=b"asset provenance version two\n",
            )
            metadata_archive, after_metadata = release_inputs.package(candidate=True)
            assert (after_metadata) == (original)
            assert (metadata_archive) != (candidate_archive)
            release_inputs.package(candidate=False)

            release_inputs.publish_bundle(
                "4" * 64,
                apk=b"application version two\n",
                metadata=b"asset provenance version two\n",
            )
            _apk_archive, after_apk = release_inputs.package(candidate=True)
            assert (after_apk) != (original)
            with pytest.raises(SystemExit, match="not phone-tested"):
                package_commands.package_target(release_inputs.target)

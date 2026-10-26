# SPDX-License-Identifier: GPL-2.0-only
"""Inspect release ZIPs built from controlled bundles and prepared host dependencies."""

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
from fplinux_cli.artifacts.release import ReleaseContext
from fplinux_cli.build.host_tools import linux_host, windows_driver
from fplinux_cli.build.host_tools import windows as windows_build
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment import definition
from fplinux_cli.environment import state as environment_states
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.packaging import archive as package_commands
from fplinux_cli.packaging import sources as package_sources
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
    """Own a synthetic successful build and its prepared packaging inputs."""

    def __init__(self, root: Path) -> None:
        """Replace compilation and dependency acquisition, retaining real ZIP assembly."""
        self.root = root
        self.cache = root / ".cache"
        self.target = "nokia-ta1618"
        self.context = ReleaseContext("1234567890ab", "1234567890ab" + "0" * 28, "a" * 64)
        self.snapshot = WorkspaceSnapshot((), "a" * 64, release_context=self.context)
        self.environment_recipe = "b" * 64
        self.documentation = write_offline_export(root)
        self.event_helper = (ROOT / "scripts/fplinux_cli/runtime/events.py").read_bytes()
        self.target_config: dict[str, Any] = {
            "platform": "ums9117",
            "identity": {"brand": "ACME", "product": "Demo", "hardware_codes": ["D-1"]},
            "adapter": {"boot_instructions": "Hold the left soft key while connecting USB."},
            "linux": {"root": {"kind": "initramfs"}},
            "rootfs": {"base_packages": [], "packages": []},
            "bundle": {"packages": []},
            "runtime": {"assets": {"pinmap": "assets/pinmap.bin"}},
        }
        self.release_manifest = {
            "image": "image/ramboot.bin",
            "bundle_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/spd_dump",
                "host/fplinux-usb-keyboard",
                "runtime-manifest.json",
                "apks/demo.apk",
                "assets.lock.toml",
            ],
            "runtime_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/spd_dump",
                "host/fplinux-usb-keyboard",
                "runtime-manifest.json",
            ],
            "documents": ["release/EXTRA.txt"],
        }
        self.platform = {
            "rootfs": {"packages": ["fplinux-font-terminus-6x12"]},
            "bundle": {"packages": ["demo"]},
            "host": {
                "runtime_tools": {"loader": "spd_dump", "keyboard": "fplinux-usb-keyboard"},
                "tools": [{"name": "spd_dump"}, {"name": "fplinux-usb-keyboard"}],
            },
        }
        for package in (*registration.COMMON_PACKAGES, "demo"):
            self.write_file(f"alpine/aports/{package}/APKBUILD", f"pkgname={package}\n".encode())
        for relative, contents in {
            f"targets/{self.target}/release/EXTRA.txt": b"phone instructions\n",
            f"targets/{self.target}/features/MICROSD.md": b"checkout-only phone procedures\n",
            "LICENSE": b"project license\n",
            "LICENSES/Unlicense.txt": b"host tool license\n",
            "common/60-fplinux.rules": b"SUBSYSTEM==usb\n",
            "third-party-licenses/musl/COPYRIGHT": b"musl notice\n",
            "docs/apps/SHOWCASE.md": b"checkout-only showcase procedures\n",
            "docs/features/FILE_TRANSFER.md": b"checkout-only file transfer procedures\n",
        }.items():
            self.write_file(relative, contents)
        signing_key = alpine_state.signing_public_key(self.cache)
        signing_key.parent.mkdir(parents=True)
        signing_key.write_bytes(b"test signing public key\n")
        self.signing_key = hashlib.sha256(signing_key.read_bytes()).hexdigest()
        self.linux_files = {
            relative: self.write_file(f"prepared-linux/{relative}", contents, mode=0o755)
            for relative, contents in {
                "runtime/bin/python3.14": b"prepared Linux Python\n",
                "runtime/lib/ld-musl-x86_64.so.1": b"prepared Linux loader\n",
            }.items()
        }
        self.windows_files = {
            relative: self.write_file(f"prepared-windows/{relative}", contents)
            for relative, contents in {
                "tools/spd_dump.exe": b"prepared Windows loader\n",
                "tools/libc_server.exe": b"prepared Windows bridge\n",
                "tools/libusb-1.0.dll": b"prepared Windows USB library\n",
                "tools/openssh/ssh.exe": b"prepared Windows SSH\n",
                "tools/openssh/ssh-keygen.exe": b"prepared Windows keygen\n",
                "tools/openssh/ssh-keyscan.exe": b"prepared Windows keyscan\n",
                "runtime/python.exe": b"prepared Windows Python\n",
            }.items()
        }
        self.driver_files = {
            relative: self.write_file(f"prepared-driver/{relative}", contents)
            for relative, contents in {
                "tools/driver/wdi-simple.exe": b"prepared Windows driver helper\n",
                "licenses/windows-driver/libwdi-COPYING.txt": b"driver helper license\n",
            }.items()
        }
        self.bundle = self.publish_bundle("c" * 64)

    def write_file(self, relative: str, contents: bytes, *, mode: int = 0o644) -> Path:
        """Create an owned input with an explicit file mode."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        path.chmod(mode)
        return path

    def publish_bundle(
        self,
        generation: str,
        *,
        profile: str | None = None,
        extra_files: dict[str, bytes] | None = None,
        required_boot: tuple[str, ...] = (),
    ) -> Path:
        """Publish controlled built bytes through the normal current-bundle pointer."""
        slot = self.cache / "out" / self.target
        if profile is not None:
            slot = slot / "profiles" / profile
        bundle = slot / "builds/release/bundles" / generation
        payloads = {
            "60-fplinux.rules": b"SUBSYSTEM==usb\n",
            "image/ramboot.bin": b"ramboot\n",
            "device-data.cpio": b"PRIVATE PREPARED PHONE DATA\n",
            "assets/pinmap.bin": b"pinmap\n",
            "host/spd_dump": b"built loader\n",
            "host/fplinux-usb-keyboard": b"built keyboard\n",
            "fplinux.py": b"#!/usr/bin/env python3\n",
            "Run.sh": b"#!/bin/sh\n",
            "Run.cmd": b"@echo off\r\n",
            "scripts/fplinux_cli/runtime/events.py": self.event_helper,
            "scripts/fplinux_cli/manifests/identity.py": b"identity helper\n",
            "scripts/fplinux_cli/runtime/ssh_transport.py": b"ssh helper\n",
            "platforms/demo/host/adapter.py": b"built adapter\n",
            "apks/demo.apk": b"built application\n",
            "assets.lock.toml": b"built asset manifest\n",
            **(extra_files or {}),
        }
        runtime = {
            **self.context.fields(),
            "target": self.target,
            "nand": None,
            "device_data": None,
            "host_tools": {"loader": "host/spd_dump", "keyboard": "host/fplinux-usb-keyboard"},
            "sha256": {
                relative: hashlib.sha256(contents).hexdigest()
                for relative, contents in payloads.items()
                if not relative.startswith(("apks/", "debug/"))
                and relative not in {"assets.lock.toml", "device-data.cpio", *required_boot}
            },
        }
        payloads["runtime-manifest.json"] = canonical_json_bytes(runtime)
        for relative, contents in payloads.items():
            self.write_file(
                (bundle / relative).relative_to(self.root).as_posix(),
                contents,
                mode=0o755
                if relative
                in {"host/spd_dump", "host/fplinux-usb-keyboard", "fplinux.py", "Run.sh"}
                else 0o644,
            )
        manifest = {
            **self.context.fields(),
            "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
            "kernel_artifacts": {},
            "boot_artifacts": {"required": list(required_boot), "runnable": bool(required_boot)},
            "environment_recipe": self.environment_recipe,
            "environment_content": "c" * 64,
            "apk_signing_key": self.signing_key,
            "device_identity": "f" * 64,
            "files": {relative: file_record(bundle / relative) for relative in payloads},
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": profile,
            "build_type": "release",
            "target": self.target,
            "workspace_digest": self.snapshot.recipe,
        }
        (bundle / BUILD_MANIFEST_NAME).write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(self.cache / "out", self.target, bundle, profile)
        return bundle

    def package(
        self,
        *,
        host: str = "linux",
        profile: str | None = None,
        boot: str | None = None,
    ) -> str:
        """Create real ZIPs and return the printed final runtime archive digest."""
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            package_commands.package_target(self.target, host=host, profile=profile, boot=boot)
        for line in stdout.getvalue().splitlines():
            if line.startswith("Archive SHA256: "):
                return line.removeprefix("Archive SHA256: ")
        pytest.fail("package output omitted its archive digest")

    def prepare_loader_license_sources(self) -> None:
        """Supply pinned loader archives while retaining real notice extraction."""
        digests = {}
        for name, members in {
            "board": {
                "board-release/Licenses/README": b"original board license explanation\n",
                "board-release/Licenses/gpl-2.0.txt": b"original board GPL terms\n",
            },
            "monitor": {"monitor-release/license.md": b"original secure monitor license\n"},
        }.items():
            path = self.cache / "downloads/sources" / name / f"{name}.tar.gz"
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

    def source_payload(
        self, *_arguments: object, **_options: object
    ) -> package_sources.SourcePayload:
        """Replace expensive source collection, preserving loader notice extraction."""
        notices = (
            package_sources.collect_loader_license_notices(self.target_config)
            if self.target_config["platform"] == "sc9820e"
            else {}
        )
        return package_sources.SourcePayload(
            {
                "project/source.c": b"controlled corresponding source\n",
                "project/build.sh": b"#!/bin/sh\nexit 0\n",
            },
            {"project/build.sh"},
            notices,
        )

    def package_patches(self) -> tuple[contextlib.AbstractContextManager[object], ...]:
        """Isolate host acquisition and checkout identity without replacing ZIP assembly."""
        return (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(
                package_commands, "current_release_context", return_value=self.context
            ),
            mock.patch.object(targets, "load_target", return_value=self.target_config),
            mock.patch.object(releases, "load_release", return_value=self.release_manifest),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
            mock.patch.object(workspaces, "target_workspace_snapshot", return_value=self.snapshot),
            mock.patch.object(
                definition, "environment_recipe_digest", return_value=self.environment_recipe
            ),
            mock.patch.object(
                environment_states,
                "load_environment_state",
                return_value=EnvironmentState(self.environment_recipe, "c" * 64, "c" * 64),
            ),
            mock.patch.object(linux_host, "prepare_linux_host", return_value=self.linux_files),
            mock.patch.object(
                windows_build, "prepare_windows_host", return_value=self.windows_files
            ),
            mock.patch.object(
                windows_driver, "prepare_windows_driver", return_value=self.driver_files
            ),
            mock.patch.object(package_sources, "collect_source_files", self.source_payload),
        )


@pytest.fixture
def release_inputs(tmp_path: Path) -> ReleaseArchiveInputs:
    """Keep the successful bundle and mutable checkout inputs separate for every case."""
    return ReleaseArchiveInputs(tmp_path)


def archive_contents(path: Path) -> tuple[dict[str, bytes], dict[str, int]]:
    """Read delivered members and Unix file modes from a real ZIP."""
    with zipfile.ZipFile(path) as archive:
        prefix = f"{path.stem}/"
        assert all(item.filename.startswith(prefix) for item in archive.infolist())
        files = {
            item.filename.removeprefix(prefix): archive.read(item) for item in archive.infolist()
        }
        modes = {
            item.filename.removeprefix(prefix): item.external_attr >> 16
            for item in archive.infolist()
        }
    return files, modes


class ReleaseArchiveArtifactTests:
    """Exercise archive admission, final metadata and preserved profile exports."""

    @pytest.mark.parametrize(
        ("platform", "source_only"),
        [("ums9117", False), ("ums9117", True), ("sc9820e", True)],
        ids=["ums9117-owner-build", "ums9117-source-only-build", "sc9820e-source-only-build"],
    )
    def test_archive_admits_current_build_and_rejects_changed_source_inputs(
        self, release_inputs: ReleaseArchiveInputs, platform: str, *, source_only: bool
    ) -> None:
        """Unchanged source identity admits a build; a changed workspace cannot reuse it."""
        inputs = release_inputs
        inputs.target_config["platform"] = platform
        if platform == "sc9820e":
            inputs.prepare_loader_license_sources()
        recorded_recipe = "9" * 64 if source_only else "a" * 64
        inputs.snapshot = WorkspaceSnapshot((), recorded_recipe, release_context=inputs.context)
        inputs.publish_bundle("d" * 64)
        sources_changed = False

        def package_snapshot(
            *_arguments: object, include_device_data: bool = True, **_options: object
        ) -> WorkspaceSnapshot:
            recipe = "7" * 64 if sources_changed else recorded_recipe
            if platform == "ums9117" and source_only and include_device_data:
                recipe = "8" * 64
            return WorkspaceSnapshot((), recipe, release_context=inputs.context)

        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            stack.enter_context(
                mock.patch.object(workspaces, "target_workspace_snapshot", package_snapshot)
            )
            inputs.package()
            archive = inputs.cache / "out/releases/FPLinux-nokia-ta1618-linux-x64-1234567890ab.zip"
            original = archive.read_bytes()
            sources_changed = True
            with pytest.raises(SystemExit, match="build output is stale"):
                inputs.package()
        assert archive.read_bytes() == original
        files, _modes = archive_contents(archive)
        assert json.loads(files["build-manifest.json"])["workspace_digest"] == recorded_recipe

    def test_owner_build_is_stale_when_personal_inputs_change(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """A source-only match cannot admit an owner build with different personal inputs."""
        inputs = release_inputs

        def package_snapshot(
            *_arguments: object, include_device_data: bool = True, **_options: object
        ) -> WorkspaceSnapshot:
            return WorkspaceSnapshot(
                (),
                "6" * 64 if include_device_data else "9" * 64,
                release_context=inputs.context,
            )

        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            stack.enter_context(
                mock.patch.object(workspaces, "target_workspace_snapshot", package_snapshot)
            )
            with pytest.raises(SystemExit, match="build output is stale"):
                inputs.package()
        assert not (inputs.cache / "out/releases").exists()

    @pytest.mark.parametrize("host", ["linux", "windows"])
    def test_ram_archive_final_metadata_modes_checksums_and_source_pairing(
        self, release_inputs: ReleaseArchiveInputs, host: str
    ) -> None:
        """The final ZIP inventories delivered bytes and pairs sources with its exact digest."""
        inputs = release_inputs
        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            printed_digest = inputs.package(host=host)
        archive = (
            inputs.cache / "out/releases" / f"FPLinux-nokia-ta1618-{host}-x64-1234567890ab.zip"
        )
        companion = archive.with_name(f"{archive.stem}-sources.zip")
        files, modes = archive_contents(archive)
        sources, source_modes = archive_contents(companion)
        assert printed_digest == hashlib.sha256(archive.read_bytes()).hexdigest()
        assert files["EXTRA.txt"] == b"phone instructions\n"
        assert files["LICENSE"] == b"project license\n"
        assert files["licenses/musl/COPYRIGHT"] == b"musl notice\n"
        assert files["image/ramboot.bin"] == b"ramboot\n"
        assert files["scripts/fplinux_cli/runtime/events.py"] == inputs.event_helper
        assert (
            b"After unmounting data cards and making any system card read-only"
            in files["README.txt"]
        )
        assert f"  {companion.name}\n".encode() in files["README.txt"]
        assert (
            not {
                "device-data.cpio",
                "host/fplinux-usb-keyboard",
                "CANDIDATE-NOTICE.txt",
                "DEVELOPMENT-NOTICE.txt",
            }
            & files.keys()
        )
        assert not any(
            name.startswith(("data/", "logs/", ".cache/", "apks/", "docs/", "sources/"))
            for name in files
        )
        assert b"PRIVATE PREPARED PHONE DATA" not in b"".join(files.values())
        assert sources["project/source.c"] == b"controlled corresponding source\n"
        assert sources["project/build.sh"] == b"#!/bin/sh\nexit 0\n"
        assert source_modes["project/build.sh"] == 0o100755
        assert source_modes["project/source.c"] == 0o100644
        assert json.loads(sources["DISTRIBUTION.json"]) == {
            "release_id": "1234567890ab",
            "source_commit": "1234567890ab" + "0" * 28,
            "source_snapshot": "a" * 64,
            "runtimes": [
                {
                    "archive": archive.name,
                    "target": "nokia-ta1618",
                    "host": host,
                    "identity": "f" * 64,
                    "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                }
            ],
        }
        build = json.loads(files["build-manifest.json"])
        runtime = json.loads(files["runtime-manifest.json"])
        for manifest in (build, runtime):
            assert manifest["release_id"] == "1234567890ab"
            assert manifest["source_commit"] == "1234567890ab" + "0" * 28
            assert manifest["source_snapshot"] == "a" * 64
        assert set(build["files"]) == files.keys() - {"build-manifest.json", "SHA256SUMS"}
        assert build["generation"] != "c" * 64
        for name, record in build["files"].items():
            assert record == {
                "mode": modes[name] & 0o777,
                "size": len(files[name]),
                "sha256": hashlib.sha256(files[name]).hexdigest(),
            }
        assert set(runtime["sha256"]) <= files.keys()
        for name, digest in runtime["sha256"].items():
            assert hashlib.sha256(files[name]).hexdigest() == digest
        for payload in (files, sources):
            checksums = {
                name: digest
                for line in payload["SHA256SUMS"].decode().splitlines()
                for digest, name in (line.split("  ", 1),)
            }
            assert checksums.keys() == payload.keys() - {"SHA256SUMS"}
            for name, digest in checksums.items():
                assert hashlib.sha256(payload[name]).hexdigest() == digest
        if host == "linux":
            assert files["host/spd_dump"] == b"built loader\n"
            assert files["runtime/bin/python3.14"] == b"prepared Linux Python\n"
            assert files["licenses/host-tools/Unlicense.txt"] == b"host tool license\n"
            executables = {
                "fplinux.py",
                "Run.sh",
                "host/spd_dump",
                "runtime/bin/python3.14",
                "runtime/lib/ld-musl-x86_64.so.1",
            }
            assert "Run.cmd" not in files
        else:
            assert files["tools/spd_dump.exe"] == b"prepared Windows loader\n"
            assert (
                files["licenses/windows-driver/libwdi-COPYING.txt"] == b"driver helper license\n"
            )
            executables = {name for name in files if name.endswith(".exe")} | {"fplinux.py"}
            assert not any(name.startswith("host/") for name in files)
            assert not {"Run.sh", "60-fplinux.rules"} & files.keys()
        for name, mode in modes.items():
            assert mode == (0o100755 if name in executables else 0o100644)

    def test_ram_archive_carries_frozen_runtime_sources_from_the_selected_bundle(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Checkout changes cannot replace the executable closure of a successful build."""
        inputs = release_inputs
        inputs.publish_bundle(
            "d" * 64,
            extra_files={"scripts/fplinux_cli/runtime/workflow.py": b"built workflow\n"},
        )
        inputs.write_file(
            "scripts/fplinux_cli/runtime/workflow.py", b"unbuilt checkout workflow\n"
        )
        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            inputs.package()
        archive = inputs.cache / "out/releases/FPLinux-nokia-ta1618-linux-x64-1234567890ab.zip"
        files, _modes = archive_contents(archive)
        assert files["scripts/fplinux_cli/runtime/workflow.py"] == b"built workflow\n"

    def test_archive_rejects_input_metadata_that_does_not_describe_its_built_file(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """Matching content hashes alone cannot admit a wrong recorded file size."""
        inputs = release_inputs
        manifest_path = inputs.bundle / BUILD_MANIFEST_NAME
        manifest = json.loads(manifest_path.read_bytes())
        manifest["files"]["image/ramboot.bin"]["size"] = 7
        manifest_path.write_bytes(canonical_json_bytes(manifest))
        publish_current_bundle(inputs.cache / "out", inputs.target, inputs.bundle)
        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            with pytest.raises(SystemExit, match="release input differs"):
                inputs.package()
        assert not (inputs.cache / "out/releases").exists()

    @pytest.mark.parametrize("host", ["linux", "windows"])
    def test_sc9820e_archive_delivers_built_loaders_with_their_original_notices(
        self, release_inputs: ReleaseArchiveInputs, host: str
    ) -> None:
        """The RAM ZIP retains every built loader stage and required license text."""
        inputs = release_inputs
        inputs.target_config["platform"] = "sc9820e"
        inputs.windows_files.pop("tools/libc_server.exe")
        inputs.prepare_loader_license_sources()
        stages = {
            "assets/fdl1.bin": b"built first stage\n",
            "assets/fdl2.bin": b"built board initializer\n",
            "assets/sml.bin": b"built secure monitor\n",
        }
        inputs.target_config["runtime"]["assets"] = {
            "fdl1": "assets/fdl1.bin",
            "fdl2": "assets/fdl2.bin",
            "sml": "assets/sml.bin",
        }
        for field in ("bundle_files", "runtime_files"):
            inputs.release_manifest[field] = [*inputs.release_manifest[field], *stages]
        inputs.publish_bundle("d" * 64, extra_files=stages)
        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            inputs.package(host=host)
        archive = inputs.cache / f"out/releases/FPLinux-nokia-ta1618-{host}-x64-1234567890ab.zip"
        files, modes = archive_contents(archive)
        assert {name: files[name] for name in stages} == {
            "assets/fdl1.bin": b"built first stage\n",
            "assets/fdl2.bin": b"built board initializer\n",
            "assets/sml.bin": b"built secure monitor\n",
        }
        assert files["licenses/loaders/fdl2/README"] == b"original board license explanation\n"
        assert files["licenses/loaders/fdl2/gpl-2.0.txt"] == b"original board GPL terms\n"
        assert files["licenses/loaders/sml/license.md"] == b"original secure monitor license\n"
        assert all(modes[name] == 0o100644 for name in stages)
        assert "device-data.cpio" not in files

    def test_archived_loader_events_work_without_the_source_checkout(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """An isolated interpreter imports only extracted code and reads a flushed event."""
        inputs = release_inputs
        inputs.publish_bundle(
            "d" * 64,
            extra_files={
                relative: (ROOT / relative).read_bytes()
                for relative in (
                    "scripts/fplinux_cli/__init__.py",
                    "scripts/fplinux_cli/common.py",
                    "scripts/fplinux_cli/runtime/__init__.py",
                )
            },
        )
        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            inputs.package()
        archive_path = (
            inputs.cache / "out/releases/FPLinux-nokia-ta1618-linux-x64-1234567890ab.zip"
        )
        extracted = inputs.root / "standalone"
        extracted.mkdir()
        with zipfile.ZipFile(archive_path) as archive:
            archive.extractall(extracted)  # noqa: S202 -- test-owned ZIP from controlled inputs.
        result = run_process(
            [
                sys.executable,
                "-I",
                str(ROOT / "tests/artifact/fixtures/release-package/archived_events_consumer.py"),
                str(extracted / archive_path.stem),
                str(extracted / "events.jsonl"),
            ],
            name="archived loader events",
            timeout=30,
            cwd=extracted,
            env={},
        )
        assert result.returncode == 0, result.stderr
        record = json.loads(result.stdout)
        record.pop("time")
        assert record == {
            "event": "waiting-for-device",
            "target": "nokia-ta1618",
            "profile": None,
            "build_type": "release",
        }

    def test_profile_and_boot_alias_export_one_external_root_generation(
        self, release_inputs: ReleaseArchiveInputs
    ) -> None:
        """The Linux profile export preserves system-card bytes and installed-host guidance."""
        inputs = release_inputs
        inputs.target_config["profile"] = "microsd-uboot"
        inputs.target_config["linux"]["root"]["kind"] = "external"
        inputs.release_manifest["bundle_files"] = [
            *inputs.release_manifest["bundle_files"],
            "device-data.cpio",
        ]
        inputs.snapshot = WorkspaceSnapshot((), "4" * 64, release_context=inputs.context)
        bundle = inputs.publish_bundle(
            "3" * 64,
            profile="microsd-uboot",
            extra_files={
                "FPLINUX.img.xz": b"profile whole-card image\n",
                "debug/vmlinux": b"debug\n",
            },
            required_boot=("FPLINUX.img.xz",),
        )
        (bundle / "debug/vmlinux").unlink()
        with contextlib.ExitStack() as stack:
            for patch in inputs.package_patches():
                stack.enter_context(patch)
            explicit_digest = inputs.package(profile="microsd-uboot")
            alias_digest = inputs.package(boot="microsd")
        archives = list((inputs.cache / "out/releases").glob("*.zip"))
        assert len(archives) == 1
        assert archives[0].name == "FPLinux-nokia-ta1618-microsd-uboot-linux-x64-1234567890ab.zip"
        assert alias_digest == explicit_digest
        files, modes = archive_contents(archives[0])
        assert files["FPLINUX.img.xz"] == b"profile whole-card image\n"
        assert files["device-data.cpio"] == b"PRIVATE PREPARED PHONE DATA\n"
        assert files["apks/demo.apk"] == b"built application\n"
        assert files["host/fplinux-usb-keyboard"] == b"built keyboard\n"
        assert files["EXTRA.txt"] == b"phone instructions\n"
        assert {name: files[name] for name in inputs.documentation} == inputs.documentation
        assert "debug/vmlinux" not in files
        assert not any(name.startswith("runtime/") for name in files)
        assert b"install Python 3.14" in files["README.txt"]
        assert b"python3.14 -I fplinux.py run\n" in files["README.txt"]
        assert b"mount -o remount,ro /" in files["README.txt"]
        assert modes["FPLINUX.img.xz"] == 0o100644
        assert modes["fplinux.py"] == 0o100755
        manifest = json.loads(files["build-manifest.json"])
        assert manifest["profile"] == "microsd-uboot"
        assert manifest["workspace_digest"] == "4" * 64
        assert manifest["files"]["FPLINUX.img.xz"] == {
            "mode": 0o644,
            "size": 25,
            "sha256": hashlib.sha256(b"profile whole-card image\n").hexdigest(),
        }

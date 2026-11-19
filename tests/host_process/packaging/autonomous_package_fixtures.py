# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic built files and native-tool boundaries shared by archive cases."""

from __future__ import annotations

import hashlib
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.alpine import selection
from fplinux_cli.artifacts.bundles import CurrentBundle
from fplinux_cli.artifacts.release import (
    PublicSourceSnapshot,
    ReleaseContext,
    public_source_digest,
)
from fplinux_cli.build import workflow
from fplinux_cli.build.host_tools import linux_host, windows_driver
from fplinux_cli.build.host_tools import windows as windows_build
from fplinux_cli.build.identity import runtime_identity
from fplinux_cli.build.runtime import device_data_descriptor, runtime_source_files
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging import distribution
from fplinux_cli.packaging import sources as package_sources
from fplinux_cli.workspace.capture import WorkspaceFile

from tests.bundle_support import file_record
from tests.fixtures.artifact_footprint import newc
from tests.fixtures.runtime_manifest import runtime_manifest
from tests.host_process.runtime.sc9820e.fixtures import prepare_runtime
from tests.small.runtime.adapter_fixtures import adapter_data

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path

KERNEL = b"built kernel bytes\n"
SQUASHFS = b"hsqs synthetic system"
RAM_ROOT = newc([("root.squashfs", SQUASHFS, 0o100644, 1, 1)])


@dataclass(frozen=True)
class ArchivePhone:
    """Select one phone and whether the fixture requires personal preparation."""

    target: str
    required_phone_data: bool = True


@dataclass(frozen=True)
class ArchiveInputs:
    """Synthetic bundle and prepared tools owned by one archive case."""

    root: Path
    target: str
    bundle: Path
    manifest: dict[str, Any]
    snapshot: PublicSourceSnapshot
    build_calls: list[tuple[str, int, bool, ReleaseContext]]
    linux_files: dict[str, Path]
    windows_scripts: dict[str, bytes]

    @property
    def output(self) -> Path:
        """Return the destination owned by the package case."""
        return self.root / "release"

    def package(self, host: str) -> Path:
        """Invoke the production package path against controlled build inputs."""
        start = len(self.build_calls)
        distribution.package_release(
            target=self.target, host=host, jobs=1, output_directory=self.output
        )
        assert self.build_calls[start:] == [(self.target, 1, False, self.snapshot.release)]
        return self.output / f"FPLinux-{host}-x64-1234567890ab.zip"


def write_archive_file(root: Path, relative: str, contents: bytes, *, mode: int = 0o644) -> Path:
    """Write fixture bytes with the mode declared by their archive case."""
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)
    path.chmod(mode)
    return path


@pytest.fixture
def archive_inputs(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, request: pytest.FixtureRequest
) -> ArchiveInputs:
    """Replace compilation and host identity, retaining real collection and ZIP writing."""
    selected = getattr(request, "param", "nokia-ta1618")
    phone = ArchivePhone(selected) if isinstance(selected, str) else selected
    return build_archive_inputs(tmp_path, monkeypatch, phone)


def build_archive_inputs(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    phone: ArchivePhone,
    *,
    emmc_files: Mapping[str, Path] | None = None,
) -> ArchiveInputs:
    """Prepare a controlled build with an optional real eMMC reader closure."""
    source = common.ROOT
    target = phone.target
    config = targets.load_target(target)
    if not phone.required_phone_data:
        config["device_data"] = {"parser": None, "groups": {}}
    platform = platforms.load_platform(config["platform"])
    rootfs_packages = selection.selected_packages(platform, config)
    selected_bundle_packages = selection.bundle_packages(platform, config, rootfs_packages)
    lock = {
        "runtime": {
            "packages": ["openrc-1-r0.apk"],
            "additions": {"fplinux-phoneme": ["fixture-runtime-2-r0.apk"]},
        }
    }
    bundle_apk_packages = (
        *selected_bundle_packages,
        *alpine_lock.bundle_runtime_packages(lock, rootfs_packages, selected_bundle_packages),
    )
    release = releases.load_release(target, bundle_packages=bundle_apk_packages)
    source_files = [
        "LICENSE",
        "LICENSES/Unlicense.txt",
        "THIRD_PARTY_NOTICES.md",
        "fplinux",
        "third-party-licenses/musl/COPYRIGHT",
        "common/60-fplinux.rules",
        "common/runtime.py",
        "common/Run.sh",
        "common/Run.cmd",
        "common/Run-debug.cmd",
        "include/fplinux/fplinux-cli.h",
        "lib/fplinux-shared/fplinux-cli.c",
    ]
    if config["platform"] == "ums9117":
        source_files.extend(
            [
                "platforms/ums9117/host/nand_ecc.py",
                "platforms/ums9117/host/nand-ecc/Makefile",
                "platforms/ums9117/host/nand-ecc/ums9117-nand-ecc.c",
            ]
        )
        parser = config["device_data"]["parser"]
        if parser is not None:
            source_files.append(f"targets/{target}/{parser}")
    for relative in source_files:
        write_archive_file(tmp_path, relative, (source / relative).read_bytes())
    release_document = f"targets/{target}/release/EXTRA.txt"
    write_archive_file(tmp_path, release_document, b"phone instructions\n")
    release["documents"] = ["release/EXTRA.txt"]
    target_manifest = f"targets/{target}/target.toml"
    write_archive_file(tmp_path, target_manifest, f'platform = "{config["platform"]}"\n'.encode())
    write_archive_file(
        tmp_path,
        "profiles/default/profile.toml",
        (source / "profiles/default/profile.toml").read_bytes(),
    )
    for name in ("alpine.lock.toml", "sources.lock.toml"):
        write_archive_file(tmp_path, name, (source / "tests/fixtures/alpine" / name).read_bytes())
    shutil.copytree(source / "scripts/fplinux_cli", tmp_path / "scripts/fplinux_cli")
    shutil.copytree(source / "alpine/aports", tmp_path / "alpine/aports")
    windows_scripts = {}
    for resource in sorted((source / "common/windows").rglob("*.ps1")):
        relative = resource.relative_to(source).as_posix()
        contents = resource.read_bytes()
        windows_scripts[relative] = contents
        write_archive_file(tmp_path, relative, contents)
    generation = "c" * 64
    bundle = tmp_path / ".data/out" / target / "builds/release/bundles" / generation
    monkeypatch.setattr(package, "load_alpine_lock", lambda _root: lock)
    normalized = package.load_release_manifest(target, config)
    runtime_sources = runtime_source_files(target, config, root=source)
    if emmc_files is not None:
        runtime_sources.update(emmc_files)
    elif config.get("emmc") is not None and device_data_descriptor(target, config) is not None:
        # Admission tests do not run the filesystem reader.
        for name in ("ext4", "cachetools", "crcmod"):
            for relative, contents in (
                (f"scripts/{name}/__init__.py", b"NAME = 'test-only dependency'\n"),
                (f"licenses/phone-data/{name}-LICENSE.txt", b"test-only dependency notice\n"),
            ):
                runtime_sources[relative] = write_archive_file(tmp_path, relative, contents)
    bundle_files = [*normalized["bundle_files"], *runtime_sources]
    if device_data_descriptor(target, config) is not None and (config.get("nand") or {}).get(
        "ecc_format"
    ):
        bundle_files.append("host/ums9117-nand-ecc")
    for relative in bundle_files:
        contents = (
            b"BUILDER PERSONAL DATA MUST STAY PRIVATE\n"
            if relative == "device-data.cpio"
            else f"synthetic built file: {relative}\n".encode()
        )
        mode = (
            0o755
            if relative
            in {
                "fplinux.py",
                "Run.sh",
                "host/spd_dump",
                "host/libc_server",
                "host/fplinux-usb-keyboard",
                "host/ums9117-nand-ecc",
            }
            else 0o644
        )
        write_archive_file(bundle, relative, contents, mode=mode)
    for relative, source_path in runtime_sources.items():
        write_archive_file(
            bundle,
            relative,
            source_path.read_bytes(),
            mode=0o755 if relative in {"fplinux.py", "Run.sh"} else 0o644,
        )
    image = b"DHTB synthetic built RAM payload\n"
    runtime = runtime_manifest()
    if config["platform"] == "sc9820e":
        sc9820e_root = tmp_path / "sc9820e-runtime"
        sc9820e_root.mkdir()
        sc9820e_runtime, _session, assets = prepare_runtime(sc9820e_root)
        runtime.update(
            {
                key: sc9820e_runtime[key]
                for key in ("adapter", "assets", "addresses", "personalization")
            }
        )
        template = bytearray((sc9820e_root / "ramboot.bin").read_bytes())
        slot = runtime["personalization"]["offset"]
        template[slot : slot + 512] = bytes(512)
        runtime["personalization"]["template_sha256"] = hashlib.sha256(bytes(512)).hexdigest()
        image = bytes(template)
        for role, relative in config["runtime"]["assets"].items():
            write_archive_file(bundle, relative, (assets / f"{role}.bin").read_bytes())
    else:
        runtime["adapter"] = adapter_data()
    for relative, contents in {
        "image/ramboot.bin": image + KERNEL + RAM_ROOT,
        "debug/zImage": KERNEL,
        "debug/initramfs.cpio": RAM_ROOT,
        "THIRD_PARTY_NOTICES.md": (source / "THIRD_PARTY_NOTICES.md").read_bytes(),
    }.items():
        write_archive_file(bundle, relative, contents)
        if relative not in bundle_files:
            bundle_files.append(relative)
    runtime["identity"] = runtime_identity(
        config["identity"], config["platform"], platform["identity"]
    )
    runtime["assets"] = dict(config["runtime"]["assets"])
    runtime["target"] = target
    runtime["host_tools"] = {
        role: f"host/{name}" for role, name in platform["host"]["runtime_tools"].items()
    }
    runtime["device_data"] = device_data_descriptor(target, config)
    if runtime["device_data"] is not None:
        runtime["device_data"]["preparation_recipe"] = "7" * 64
    runtime["nand"] = (
        {**config["nand"], "acquisition_recipe": "8" * 64} if config.get("nand") else None
    )
    runtime["emmc"] = (
        {**config["emmc"], "acquisition_recipe": "9" * 64} if config.get("emmc") else None
    )
    if runtime["device_data"] is not None and runtime["device_data"]["ecc_module"] is not None:
        runtime["host_tools"]["nand_ecc"] = "host/ums9117-nand-ecc"
        write_archive_file(bundle, "host/ums9117-nand-ecc", b"prepared BCH decoder", mode=0o755)
    runtime["sha256"] = {
        name: hashlib.sha256((bundle / name).read_bytes()).hexdigest()
        for name in (
            "image/ramboot.bin",
            *runtime["assets"].values(),
            *runtime_sources,
            *runtime["host_tools"].values(),
        )
    }
    project = (
        WorkspaceFile("LICENSE", b"project license\n", 0o644),
        WorkspaceFile("THIRD_PARTY_NOTICES.md", b"project notices\n", 0o644),
        WorkspaceFile("fplinux", b"#!/bin/sh\nexit 0\n", 0o755),
        WorkspaceFile(target_manifest, (tmp_path / target_manifest).read_bytes(), 0o644),
    )
    context = ReleaseContext(
        "1234567890ab", "1234567890ab" + "0" * 28, public_source_digest(project)
    )
    snapshot = PublicSourceSnapshot(project, context)
    build_calls: list[tuple[str, int, bool, ReleaseContext]] = []
    runtime.update(context.fields())
    write_archive_file(bundle, "runtime-manifest.json", canonical_json_bytes(runtime))
    manifest: dict[str, Any] = {
        **context.fields(),
        "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
        "kernel_artifacts": {"image": "debug/zImage", "initramfs": "debug/initramfs.cpio"},
        "boot_artifacts": {"required": []},
        "environment_recipe": "b" * 64,
        "environment_content": "c" * 64,
        "apk_signing_key": "3" * 64,
        "device_identity": "f" * 64,
        "files": {relative: file_record(bundle / relative) for relative in bundle_files},
        "generation": generation,
        "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
        "linux_recipe": "2" * 64,
        "profile": None,
        "build_type": "release",
        "target": target,
        "workspace_digest": "a" * 64,
    }
    manifest_bytes = canonical_json_bytes(manifest)
    write_archive_file(bundle, "build-manifest.json", manifest_bytes)
    native_files = {
        relative: write_archive_file(tmp_path / "prepared-native", relative, relative.encode())
        for relative in (
            "tools/spd_dump.exe",
            "tools/libc_server.exe",
            "tools/libusb-1.0.dll",
            "tools/openssh/ssh.exe",
            "tools/openssh/ssh-keygen.exe",
            "tools/openssh/ssh-keyscan.exe",
            "runtime/python.exe",
            "runtime/python314.zip",
        )
    }
    native_files["sources/windows-tools-source.zip"] = write_archive_file(
        tmp_path / "prepared-native",
        "sources/windows-tools-source.zip",
        b"prepared compiler source projection",
    )
    driver_files = {
        "tools/driver/wdi-simple.exe": write_archive_file(
            tmp_path / "prepared-driver", "tools/driver/wdi-simple.exe", b"prepared driver helper"
        ),
        "licenses/windows-driver/License.rtf": write_archive_file(
            tmp_path / "prepared-driver",
            "licenses/windows-driver/License.rtf",
            b"synthetic driver redistribution license\n",
        ),
        **{
            f"sources/windows-driver/{name}": write_archive_file(
                tmp_path / "prepared-driver", f"sources/windows-driver/{name}", b"fixture source\n"
            )
            for name in ("README.txt", "build.py", "downloads.json")
        },
    }
    linux_files = {
        relative: write_archive_file(
            tmp_path / "prepared-linux", relative, b"prepared Linux runtime", mode=0o755
        )
        for relative in ("runtime/bin/python3.14", "runtime/lib/ld-musl-x86_64.so.1")
    }

    def prepared_native(**_options: object) -> dict[str, Path]:
        """Stand in for the separate, expensive cross-compilation boundary."""
        assert _options["platform"] == config["platform"]
        files = dict(native_files)
        if _options["platform"] == "sc9820e":
            files.pop("tools/libc_server.exe")
        if _options.get("nand_ecc"):
            files["tools/ums9117-nand-ecc.exe"] = write_archive_file(
                tmp_path / "prepared-native", "tools/ums9117-nand-ecc.exe", b"prepared BCH decoder"
            )
            files["licenses/windows-host/ums9117-nand-ecc-COPYING.txt"] = write_archive_file(
                tmp_path / "prepared-native",
                "licenses/windows-host/ums9117-nand-ecc-COPYING.txt",
                b"prepared BCH license",
            )
        return files

    def prepared_driver(**_options: object) -> dict[str, Path]:
        """Stand in for the locked driver build and redistributable preparation."""
        return dict(driver_files)

    def prepared_linux(**_options: object) -> dict[str, Path]:
        """Stand in for the exact pinned-environment runtime export boundary."""
        return dict(linux_files)

    def captured_sources(*, selected_data_root: Path) -> PublicSourceSnapshot:
        """Replace source identity capture while requiring the selected archive store."""
        assert selected_data_root == tmp_path / ".data"
        return snapshot

    def built_bundle(
        name: str, jobs: int, *, include_device_data: bool, release_context: ReleaseContext
    ) -> tuple[CurrentBundle, dict[str, Any]]:
        build_calls.append((name, jobs, include_device_data, release_context))
        admitted = CurrentBundle(
            bundle, generation, hashlib.sha256(manifest_bytes).hexdigest(), manifest_bytes
        )
        return admitted, manifest

    def corresponding_sources(
        selected: dict[str, Any], *, host: str, **_options: object
    ) -> package_sources.SourcePayload:
        member = "upstream/test-package.tar.gz"
        contents = b"test upstream archive\n"
        downloads = [
            {
                "archive": member,
                "algorithm": "sha256",
                "checksum": hashlib.sha256(contents).hexdigest(),
            }
        ]
        files = {
            member: contents,
            "upstream/downloads.json": canonical_json_bytes(downloads),
            "upstream/alpine-source-channels.json": b"[]\n",
        }
        if host == "linux":
            files["BUILD-LINUX.txt"] = b"fixture rebuild instructions\n"
        else:
            files["upstream/windows-runtime-source-channels.json"] = b"[]\n"
        notices = (
            {
                "licenses/loaders/fdl2/README": b"original license explanation\n",
                "licenses/loaders/fdl2/gpl-2.0.txt": b"original GPL terms\n",
                "licenses/loaders/sml/license.md": b"original monitor license\n",
            }
            if selected["platform"] == "sc9820e"
            else {}
        )
        return package_sources.SourcePayload(files, set(), notices)

    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(tmp_path / ".data"))
    monkeypatch.setattr(distribution, "capture_public_sources", captured_sources)
    monkeypatch.setattr(workflow, "build", built_bundle)
    monkeypatch.setattr(targets, "load_target", lambda *_args, **_kwargs: config)
    monkeypatch.setattr(releases, "load_release", lambda *_args, **_kwargs: release)
    monkeypatch.setattr(platforms, "load_platform", lambda *_args: platform)
    monkeypatch.setattr(windows_build, "prepare_windows_host", prepared_native)
    monkeypatch.setattr(windows_driver, "prepare_windows_driver", prepared_driver)
    monkeypatch.setattr(linux_host, "prepare_linux_host", prepared_linux)
    monkeypatch.setattr(package_sources, "collect_source_files", corresponding_sources)
    return ArchiveInputs(
        root=tmp_path,
        target=target,
        bundle=bundle,
        manifest=manifest,
        snapshot=snapshot,
        build_calls=build_calls,
        linux_files=linux_files,
        windows_scripts=windows_scripts,
    )

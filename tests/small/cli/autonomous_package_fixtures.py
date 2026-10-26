# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic built files and native-tool boundaries shared by archive cases."""

from __future__ import annotations

import hashlib
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import selection, signing
from fplinux_cli.artifacts.bundles import publish_current_bundle
from fplinux_cli.build.host_tools import linux_host, windows_driver
from fplinux_cli.build.host_tools import windows as windows_build
from fplinux_cli.build.identity import runtime_identity
from fplinux_cli.build.runtime import device_data_descriptor, runtime_source_files
from fplinux_cli.artifacts.release import ReleaseContext
from fplinux_cli.common import canonical_json_bytes
from fplinux_cli.environment import definition
from fplinux_cli.environment import state as environment_states
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging import sources as package_sources
from fplinux_cli.workspace import build_inputs
from fplinux_cli.workspace.capture import WorkspaceSnapshot

from tests.bundle_support import file_record
from tests.fixtures.runtime_manifest import runtime_manifest
from tests.small.cli.package_docs_fixtures import write_offline_export
from tests.small.runtime.adapter_fixtures import adapter_data

if TYPE_CHECKING:
    from pathlib import Path


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
    manifest: bytes
    native_files: dict[str, Path]
    linux_files: dict[str, Path]
    windows_scripts: dict[str, bytes]


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
    source = common.ROOT
    selected = getattr(request, "param", "nokia-ta1618")
    phone = ArchivePhone(selected) if isinstance(selected, str) else selected
    target = phone.target
    config = targets.load_target(target)
    if not phone.required_phone_data:
        config["device_data"] = {"parser": None, "groups": {}}
    platform = platforms.load_platform(config["platform"])
    rootfs_packages = selection.selected_packages(platform, config)
    selected_bundle_packages = selection.bundle_packages(platform, config, rootfs_packages)
    release = releases.load_release(target, bundle_packages=selected_bundle_packages)
    source_files = [
        "LICENSE",
        "LICENSES/Unlicense.txt",
        "third-party-licenses/musl/COPYRIGHT",
        "common/60-fplinux.rules",
        "common/runtime.py",
        "common/Run.sh",
        "common/Run.cmd",
        "include/fplinux/fplinux-cli.h",
        "lib/fplinux/fplinux-cli.c",
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
    shutil.copytree(source / "scripts/fplinux_cli", tmp_path / "scripts/fplinux_cli")
    shutil.copytree(source / "alpine/aports", tmp_path / "alpine/aports")
    windows_scripts = {}
    for resource in sorted((source / "common/windows").rglob("*.ps1")):
        relative = resource.relative_to(source).as_posix()
        contents = resource.read_bytes()
        windows_scripts[relative] = contents
        write_archive_file(tmp_path, relative, contents)
    public_key = signing.signing_public_key(tmp_path / ".cache")
    write_archive_file(
        tmp_path, public_key.relative_to(tmp_path).as_posix(), b"test public signing key\n"
    )
    signing_digest = hashlib.sha256(public_key.read_bytes()).hexdigest()
    generation = "c" * 64
    bundle = tmp_path / ".cache/out" / target / "builds/release/bundles" / generation
    normalized = package.load_release_manifest(target, config)
    runtime_sources = runtime_source_files(target, config, root=source)
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
    write_archive_file(bundle, "image/ramboot.bin", b"DHTB synthetic built RAM payload\n")
    runtime = runtime_manifest()
    runtime["identity"] = runtime_identity(
        config["identity"], config["platform"], platform["identity"]
    )
    runtime["adapter"] = (
        dict(config["runtime"]["adapter"]) if config["platform"] == "sc9820e" else adapter_data()
    )
    runtime["assets"] = dict(config["runtime"]["assets"])
    runtime["target"] = target
    runtime["host_tools"] = {
        role: f"host/{name}" for role, name in platform["host"]["runtime_tools"].items()
    }
    runtime["device_data"] = device_data_descriptor(target, config)
    runtime["nand"] = config.get("nand")
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
    context = ReleaseContext("1234567890ab", "1234567890ab" + "0" * 28, "a" * 64)
    runtime.update(context.fields())
    write_archive_file(bundle, "runtime-manifest.json", canonical_json_bytes(runtime))
    snapshot = WorkspaceSnapshot((), "a" * 64, release_context=context)
    manifest = canonical_json_bytes(
        {
            **context.fields(),
            "rootfs_receipt": {"recipe": "d" * 64, "sha256": "e" * 64},
            "kernel_artifacts": {},
            "boot_artifacts": {"required": []},
            "environment_recipe": "b" * 64,
            "environment_content": "c" * 64,
            "apk_signing_key": signing_digest,
            "device_identity": "f" * 64,
            "files": {relative: file_record(bundle / relative) for relative in bundle_files},
            "generation": generation,
            "kbuild_receipt": {"recipe": "0" * 64, "sha256": "1" * 64},
            "linux_recipe": "2" * 64,
            "profile": None,
            "build_type": "release",
            "target": target,
            "workspace_digest": snapshot.recipe,
        }
    )
    write_archive_file(bundle, "build-manifest.json", manifest)
    publish_current_bundle(tmp_path / ".cache/out", target, bundle)
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
    driver_files = {
        "tools/driver/wdi-simple.exe": write_archive_file(
            tmp_path / "prepared-driver", "tools/driver/wdi-simple.exe", b"prepared driver helper"
        ),
        "licenses/windows-driver/License.rtf": write_archive_file(
            tmp_path / "prepared-driver",
            "licenses/windows-driver/License.rtf",
            b"synthetic driver redistribution license\n",
        ),
    }
    linux_files = {
        relative: write_archive_file(
            tmp_path / "prepared-linux", relative, b"prepared Linux runtime", mode=0o755
        )
        for relative in ("runtime/bin/python3.14", "runtime/lib/ld-musl-x86_64.so.1")
    }

    def prepared_native(**_options: object) -> dict[str, Path]:
        """Stand in for the separate, expensive cross-compilation boundary."""
        files = dict(native_files)
        if _options.get("nand_ecc"):
            files["tools/ums9117-nand-ecc.exe"] = write_archive_file(
                tmp_path / "prepared-native", "tools/ums9117-nand-ecc.exe", b"prepared BCH decoder"
            )
            files["sources/windows-tools-source.zip"] = write_archive_file(
                tmp_path / "prepared-native",
                "sources/windows-tools-source.zip",
                b"prepared compiler source projection",
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

    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setattr(targets, "load_target", lambda *_args, **_kwargs: config)
    monkeypatch.setattr(releases, "load_release", lambda *_args, **_kwargs: release)
    monkeypatch.setattr(platforms, "load_platform", lambda *_args: platform)
    monkeypatch.setattr(
        build_inputs, "target_workspace_snapshot", lambda *_args, **_kwargs: snapshot
    )
    monkeypatch.setattr(definition, "environment_recipe_digest", lambda: "b" * 64)
    monkeypatch.setattr(
        environment_states,
        "load_environment_state",
        lambda *_args: EnvironmentState("b" * 64, "d" * 64, "c" * 64),
    )
    monkeypatch.setattr(windows_build, "prepare_windows_host", prepared_native)
    monkeypatch.setattr(windows_driver, "prepare_windows_driver", prepared_driver)
    monkeypatch.setattr(linux_host, "prepare_linux_host", prepared_linux)
    monkeypatch.setattr(
        package_sources,
        "collect_source_files",
        lambda *_args, **_kwargs: package_sources.SourcePayload(
            {"project/source.c": b"controlled corresponding source\n"}, set(), {}
        ),
    )
    write_offline_export(tmp_path)
    return ArchiveInputs(
        root=tmp_path,
        target=target,
        bundle=bundle,
        manifest=manifest,
        native_files={**native_files, **driver_files},
        linux_files=linux_files,
        windows_scripts=windows_scripts,
    )

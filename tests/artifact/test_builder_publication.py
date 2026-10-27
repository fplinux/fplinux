# SPDX-License-Identifier: GPL-2.0-only
"""Artifact-level tests for immutable bundle publication."""

from __future__ import annotations

import hashlib
import json
import os
import struct
from dataclasses import dataclass, replace
from typing import TYPE_CHECKING, Any, cast
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.artifacts.bundles import (
    BUILD_MANIFEST_NAME,
    bundle_generations,
    create_bundle_staging,
    resolve_current_bundle,
)
from fplinux_cli.artifacts.release import ReleaseContext
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import publish as publish_build
from fplinux_cli.build.runtime import runtime_source_files
from fplinux_cli.runtime.bundle import admit_bundle

from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.bootstrap.result import PayloadRegion


@dataclass(frozen=True)
class PublicationInputs:
    """Complete builder inputs and the selected immutable publication slot."""

    root: Path
    output: Path
    work: Path
    asset_lock: Path
    host_tool: Path
    kernel: Path
    zimage: Path
    dtb: Path
    ramboot: Path
    ramboot_map: Path
    rootfs_output: Path
    rootfs_recipe: str
    rootfs: Path
    initramfs: Path
    bundle_apks: dict[str, Path]
    target_config: dict[str, Any]
    platform: dict[str, Any]
    release_manifest: dict[str, Any]
    environment: dict[str, str]

    def publish(
        self,
        ramboot: Path | None = None,
        bundle_packages: tuple[str, ...] | None = None,
        personalization: dict[str, int | str] | None = None,
        payload_regions: dict[str, PayloadRegion] | None = None,
        *,
        asset_outputs: dict[str, tuple[str, str]] | None = None,
    ) -> Path:
        """Publish the configured fixture through the builder entry point."""
        with mock.patch.dict(os.environ, self.environment, clear=False):
            return publish_build.publish_bundle(
                target="demo",
                target_config=self.target_config,
                platform=self.platform,
                release_manifest=self.release_manifest,
                work=self.work,
                rootfs=self.rootfs,
                linux_source=self.kernel,
                kernel_output=self.kernel,
                zimage=self.zimage,
                dtb=self.dtb,
                ramboot=self.ramboot if ramboot is None else ramboot,
                ramboot_map=self.ramboot_map,
                personalization=(
                    {
                        "offset": 1024,
                        "bytes": 512,
                        "template_sha256": hashlib.sha256(bytes(512)).hexdigest(),
                    }
                    if personalization is None
                    else personalization
                ),
                asset_lock_path=self.asset_lock,
                asset_outputs=(
                    {
                        "pin": (
                            "pin.bin",
                            hashlib.sha256(
                                (self.work / "assets/pin.bin").read_bytes()
                            ).hexdigest(),
                        )
                    }
                    if asset_outputs is None
                    else asset_outputs
                ),
                host_tools={"keyboard": self.host_tool},
                linux_recipe="c" * 64,
                device_identity="9" * 64,
                rootfs_output=self.rootfs_output,
                rootfs_recipe=self.rootfs_recipe,
                kbuild_receipt={"recipe": "e" * 64, "sha256": "f" * 64},
                bundle_packages=(
                    tuple(self.bundle_apks) if bundle_packages is None else bundle_packages
                ),
                bundle_apks=self.bundle_apks,
                payload_regions=payload_regions,
            )

    def staging_directories(self) -> list[Path]:
        """Return only incomplete staging trees for this test target."""
        generations = bundle_generations(self.output, "demo")
        if not generations.exists():
            return []
        return sorted(path for path in generations.iterdir() if path.name.startswith(".stage-"))

    def assert_old_current_and_no_staging(self, current: Path) -> None:
        """Check failure cleanup without disturbing the last-good selected generation."""
        assert (resolve_current_bundle(self.output, "demo").path) == (current)
        assert (self.staging_directories()) == ([])


@pytest.fixture
def publication_inputs(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> PublicationInputs:
    """Create a complete isolated builder input tree."""
    root = tmp_path / "source"
    output = tmp_path / "out"
    work = tmp_path / "work"
    root.mkdir()
    output.mkdir()
    work.mkdir()

    def write(relative: str, data: bytes, *, root: Path = root) -> Path:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    write("common/runtime.py", b"#!/usr/bin/env python3\n")
    write("common/60-fplinux.rules", b'SUBSYSTEM=="usb"\n')
    write("scripts/fplinux_cli/runtime/events.py", b"# bundled events helper\n")
    write("scripts/fplinux_cli/manifests/identity.py", b"# identity contract\n")
    write("scripts/fplinux_cli/runtime/ssh_transport.py", b"# bundled SSH helper\n")
    write("platforms/demo/host/adapter.py", b"ADAPTER = 'demo'\n")
    write("THIRD_PARTY_NOTICES.md", b"notices\n")
    asset_lock = write("assets.lock.toml", b"[asset]\n")
    write("assets/pin.bin", b"asset\n", root=work)
    host_tool = write("keyboard", b"host tool\n", root=work)
    kernel = work / "kernel"
    kernel.mkdir()
    zimage = write("zImage", b"zimage\n", root=kernel)
    dtb = write(
        "demo.dtb",
        binary_tree(
            [],
            [
                (
                    "nand-controller@21a00000",
                    [
                        ("compatible", b"fplinux,ums9117-nandc-ro\0"),
                        ("reg", struct.pack(">II", 0x21A00000, 0x1000)),
                        ("sprd,aon-apb", struct.pack(">I", 1)),
                    ],
                    [],
                ),
                (
                    "syscon@402e0000",
                    [
                        ("compatible", b"sprd,ums9117-aon-apb\0syscon\0"),
                        ("reg", struct.pack(">II", 0x402E0000, 0x1000)),
                        ("phandle", struct.pack(">I", 1)),
                    ],
                    [],
                ),
            ],
        ),
        root=kernel,
    )
    for relative in (
        "drivers/memory/ums9117/ums9117-nandc-ro.c",
        "drivers/memory/ums9117/ums9117-nandc-chip.c",
        "drivers/memory/ums9117/ums9117-nandc-chip.h",
        "drivers/memory/ums9117/ums9117-nandc-feature.c",
        "drivers/memory/ums9117/ums9117-nandc-feature.h",
        "include/linux/soc/ums9117/mmio.h",
    ):
        write(relative, b"/* prepared reader input */\n", root=kernel)
    write("vmlinux", b"vmlinux\n", root=kernel)
    write("System.map", b"map\n", root=kernel)
    write(".config", b"CONFIG_TEST=y\n", root=kernel)
    write("device-data.cpio", b"prepared device data\n", root=work)
    ramboot = write("ramboot.bin", b"ramboot\n", root=work)
    ramboot_map = write("ramboot.map", b"map\n", root=work)
    rootfs_output = work / "rootfs"
    rootfs_recipe = "d" * 64
    rootfs = write("rootfs.cpio", b"logical composition\n", root=rootfs_output)
    initramfs = write("initramfs.cpio", b"boot archive\n", root=rootfs_output)
    bundle_apks = {
        "fplinux-base-ui": write(
            "packages/fplinux-base-ui-1-r0.apk",
            b"base apk\n",
            root=work,
        ),
        "fplinux-demo-ui": write(
            "packages/fplinux-demo-ui-3-r1.apk",
            b"target apk\n",
            root=work,
        ),
    }
    target_config: dict[str, Any] = {
        "build_type": "release",
        "identity": {
            "brand": "Demo",
            "product": "Phone",
            "hardware_codes": [],
            "compatible": "demo,phone",
            "display_name": "Demo Phone",
        },
        "platform": "demo",
        "bundle": {"packages": list(bundle_apks)},
        "linux": {"debug_dtb": "demo.dtb", "root": {"kind": "initramfs"}},
        "bootstrap": {"load_address": 2, "initramfs_delivery": "embedded"},
        "runtime": {
            "fdl1_load_address": 1,
            "usb": {"kind": "test"},
            "adapter": {"kind": "test"},
        },
    }
    platform = {
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
    runtime_sources = runtime_source_files("demo", target_config, root=root)
    for source in runtime_sources.values():
        if not source.exists():
            write(source.relative_to(root).as_posix(), b"# standalone runtime source\n")
    release_manifest = {
        "image": "image/ramboot.bin",
        "bundle_files": [
            "image/ramboot.bin",
            "assets/pin.bin",
            "host/keyboard",
            "runtime-manifest.json",
            "assets.lock.toml",
            "THIRD_PARTY_NOTICES.md",
            "apks/fplinux-base-ui.apk",
            "apks/fplinux-demo-ui.apk",
        ],
    }
    environment = {
        "FPLINUX_WORKSPACE_DIGEST": "a" * 64,
        "FPLINUX_ENVIRONMENT_RECIPE": "b" * 64,
        "FPLINUX_ENVIRONMENT_CONTENT": "c" * 64,
    }
    monkeypatch.setattr(common, "ROOT", root)
    monkeypatch.setattr(inputs_build, "OUTPUT", output)
    monkeypatch.setattr(
        publish_build,
        "trusted_receipt_identity",
        mock.Mock(return_value={"recipe": rootfs_recipe, "sha256": "8" * 64}),
    )
    monkeypatch.setattr(publish_build, "signing_key_identity", mock.Mock(return_value="7" * 64))
    return PublicationInputs(
        root=root,
        output=output,
        work=work,
        asset_lock=asset_lock,
        host_tool=host_tool,
        kernel=kernel,
        zimage=zimage,
        dtb=dtb,
        ramboot=ramboot,
        ramboot_map=ramboot_map,
        rootfs_output=rootfs_output,
        rootfs_recipe=rootfs_recipe,
        rootfs=rootfs,
        initramfs=initramfs,
        bundle_apks=bundle_apks,
        target_config=target_config,
        platform=platform,
        release_manifest=release_manifest,
        environment=environment,
    )


class BuilderPublicationTests:
    """Exercise builder-facing immutable bundle publication."""

    @pytest.mark.parametrize(
        "source_commit",
        [None, "1234567890ab" + "0" * 28, "1234567890ab" + "0" * 52],
        ids=["source-archive", "git-sha1", "git-sha256"],
    )
    def test_published_release_is_admitted_by_the_delivered_runtime_contract(
        self,
        publication_inputs: PublicationInputs,
        monkeypatch: pytest.MonkeyPatch,
        source_commit: str | None,
    ) -> None:
        """The publisher and runner agree before any host setup or USB operation."""
        context = ReleaseContext("1234567890ab", source_commit, "1234567890ab" + "a" * 52)
        monkeypatch.setattr(publish_build, "current_release_context", lambda: context)
        config = publication_inputs.target_config
        config["bootstrap"]["load_address"] = 0x80100000
        config["runtime"]["fdl1_load_address"] = 0x6200
        config["runtime"]["usb"] = {
            "bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 30},
            "linux_gadget": {
                "vendor_id": 0x0525,
                "product_id": 0xA4A6,
                "wait_seconds": 30,
                "keyboard_interface": 1,
            },
        }

        published = publication_inputs.publish()
        admitted = admit_bundle(published)

        assert admitted.root == published
        assert admitted.generation == published.name
        assert admitted.device_identity == "9" * 64
        assert admitted.runtime["release_id"] == "1234567890ab"
        assert admitted.runtime["source_commit"] == source_commit
        assert admitted.runtime["source_snapshot"] == "1234567890ab" + "a" * 52

    def test_raw_image_regions_and_external_initramfs_have_actual_published_paths(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """Runtime upload metadata and footprint paths describe the produced raw artifacts."""
        image = publication_inputs.zimage.with_name("Image")
        image.write_bytes(b"raw ARM Image\n")
        inputs = replace(publication_inputs, zimage=image)
        inputs.target_config["bootstrap"]["initramfs_delivery"] = "external"
        inputs.target_config["bootstrap"]["backend"] = "sc9820e"
        loaders = {"fdl1": b"first-stage", "fdl2": b"board-init", "sml": b"secure-monitor"}
        inputs.release_manifest["bundle_files"].remove("assets/pin.bin")
        inputs.release_manifest["bundle_files"].extend(f"assets/{role}.bin" for role in loaders)
        for role, contents in loaders.items():
            (inputs.work / "assets" / f"{role}.bin").write_bytes(contents)
        regions: dict[str, PayloadRegion] = {
            "entry": {"offset": 0, "size": 12, "load_address": 0x80002060},
            "kernel": {"offset": 64, "size": 14, "load_address": 0x80008000},
            "dtb": {"offset": 128, "size": 4, "load_address": 0x85400000},
            "initramfs": {"offset": 192, "size": 12, "load_address": 0x85500000},
        }
        published = inputs.publish(
            payload_regions=regions,
            asset_outputs={
                role: (f"{role}.bin", hashlib.sha256(contents).hexdigest())
                for role, contents in loaders.items()
            },
        )
        runtime = json.loads((published / "runtime-manifest.json").read_text())
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        assert runtime["adapter"]["regions"] == regions
        assert "regions" not in inputs.target_config["runtime"]["adapter"]
        assert runtime["assets"] == {
            "fdl1": "assets/fdl1.bin",
            "fdl2": "assets/fdl2.bin",
            "sml": "assets/sml.bin",
        }
        for role, contents in loaders.items():
            assert (published / "assets" / f"{role}.bin").read_bytes() == contents
            assert runtime["sha256"][f"assets/{role}.bin"] == hashlib.sha256(contents).hexdigest()
        assert (published / "debug/Image").read_bytes() == b"raw ARM Image\n"
        assert not (published / "debug/zImage").exists()
        assert manifest["kernel_artifacts"] == {
            "image": "debug/Image",
            "config": "debug/kernel.config",
            "vmlinux": "debug/vmlinux",
            "system_map": "debug/System.map",
            "rootfs": "debug/rootfs.cpio",
            "initramfs": "debug/initramfs.cpio",
            "initramfs_delivery": "external",
        }

    def test_publish_uses_staging_and_records_every_payload_file(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """Publish a complete generation through the current pointer."""
        published = publication_inputs.publish()
        current = resolve_current_bundle(publication_inputs.output, "demo")
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        runtime = json.loads((published / "runtime-manifest.json").read_text())
        assert (published / "device-data.cpio").read_bytes() == b"prepared device data\n"
        actual_payload = {
            path.relative_to(published).as_posix(): {
                "mode": path.stat().st_mode & 0o777,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "size": path.stat().st_size,
            }
            for path in published.rglob("*")
            if path.is_file() and path.name != BUILD_MANIFEST_NAME
        }

        assert (current.path) == (published)
        assert (published.parent) == (publication_inputs.output / "demo/builds/release/bundles")
        assert (manifest["generation"]) == (published.name)
        assert (manifest["profile"]) is None
        assert (manifest["build_type"]) == ("release")
        assert (manifest["environment_recipe"]) == ("b" * 64)
        assert (manifest["environment_content"]) == ("c" * 64)
        assert (manifest["apk_signing_key"]) == ("7" * 64)
        assert (manifest["rootfs_receipt"]["recipe"]) == (publication_inputs.rootfs_recipe)
        assert (manifest["boot_artifacts"]) == (
            {
                "required": [],
                "runnable": True,
            }
        )
        assert (manifest["kbuild_receipt"]["recipe"]) == ("e" * 64)
        assert (manifest["device_identity"]) == ("9" * 64)
        assert (manifest["files"]) == (actual_payload)
        assert ((published / "apks/fplinux-base-ui.apk").read_bytes()) == (b"base apk\n")
        assert ((published / "apks/fplinux-demo-ui.apk").read_bytes()) == (b"target apk\n")
        assert ((published / "debug/rootfs.cpio").read_bytes()) == (b"logical composition\n")
        assert ((published / "debug/initramfs.cpio").read_bytes()) == (b"boot archive\n")
        assert (manifest["files"]["debug/rootfs.cpio"]["size"]) == (
            publication_inputs.rootfs.stat().st_size
        )
        assert (runtime["sha256"]["image/ramboot.bin"]) == (
            hashlib.sha256((published / "image/ramboot.bin").read_bytes()).hexdigest()
        )
        assert (set(runtime)) == (
            {
                "target",
                "profile",
                "build_type",
                "release_id",
                "source_commit",
                "source_snapshot",
                "identity",
                "transport",
                "image",
                "addresses",
                "usb",
                "personalization",
                "assets",
                "adapter",
                "host_tools",
                "nand",
                "emmc",
                "device_data",
                "sha256",
            }
        )
        assert (runtime["profile"]) is None
        assert (runtime["build_type"]) == ("release")
        assert (runtime["identity"]["target"]["display_name"]) == ("Demo Phone")
        assert (runtime["identity"]["platform"]["name"]) == ("demo")
        assert (runtime["transport"]) == ("usb-ncm")
        helper = published / "scripts/fplinux_cli/runtime/ssh_transport.py"
        assert (runtime["personalization"]["bytes"]) == (512)
        assert (runtime["assets"]) == ({"pin": "assets/pin.bin"})
        assert (runtime["sha256"]["scripts/fplinux_cli/runtime/ssh_transport.py"]) == (
            hashlib.sha256(helper.read_bytes()).hexdigest()
        )
        event_source = (
            publication_inputs.root / "scripts/fplinux_cli/runtime/events.py"
        ).read_bytes()
        assert ((published / "scripts/fplinux_cli/runtime/events.py").read_bytes()) == (
            event_source
        )
        assert (runtime["sha256"]["scripts/fplinux_cli/runtime/events.py"]) == (
            hashlib.sha256(event_source).hexdigest()
        )
        assert runtime["device_data"] is None
        assert runtime["nand"] is None
        assert runtime["emmc"] is None
        assert (published / "60-fplinux.rules").read_bytes() == b'SUBSYSTEM=="usb"\n'
        assert (
            runtime["sha256"]["60-fplinux.rules"]
            == hashlib.sha256(b'SUBSYSTEM=="usb"\n').hexdigest()
        )
        assert (published / "fplinux.py").read_bytes() == b"#!/usr/bin/env python3\n"
        assert (published / "platforms/demo/host/adapter.py").read_bytes() == b"ADAPTER = 'demo'\n"
        assert not (published / "runner").exists()

    @pytest.mark.parametrize("root_kind", ["initramfs", "external"])
    def test_publication_keeps_capsules_only_for_ram_roots(
        self, publication_inputs: PublicationInputs, root_kind: str
    ) -> None:
        """External-root data belongs to its built rootfs, RAM roots keep the capsule."""
        publication_inputs.target_config["linux"]["root"] = {"kind": root_kind}
        publication_inputs.target_config["platform"] = "ums9117"
        publication_inputs.target_config["nand"] = {"raw_device": "/dev/raw-nand"}
        publication_inputs.target_config["bootstrap"]["initramfs_delivery"] = (
            "embedded" if root_kind == "initramfs" else "none"
        )
        for source in runtime_source_files(
            "demo", publication_inputs.target_config, root=publication_inputs.root
        ).values():
            if not source.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b"# independent NAND runtime\n")

        published = publication_inputs.publish()
        assert (published / "device-data.cpio").exists() is (root_kind == "initramfs")
        runtime = json.loads((published / "runtime-manifest.json").read_bytes())
        assert runtime["device_data"] is None
        assert runtime["nand"]["raw_device"] == "/dev/raw-nand"
        assert len(runtime["nand"]["acquisition_recipe"]) == 64

    def test_runtime_preparation_captures_the_declared_parser_without_board_maps(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """The bundle owns the parser bytes and the exact requested preparation groups."""
        config = publication_inputs.target_config
        config["platform"] = "ums9117"
        config["device_data"] = {
            "parser": "phone_data.py",
            "groups": {"audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}]},
        }
        config["audio_policy"] = {
            "speaker_vibration": False,
            "speaker_from_headphones": False,
            "headfree_headphones_only": False,
        }
        config["nand"] = {"raw_device": "/dev/raw-nand"}
        for source in runtime_source_files("demo", config, root=publication_inputs.root).values():
            if not source.exists():
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b"# declared preparation source\n")

        published = publication_inputs.publish()
        runtime = json.loads((published / "runtime-manifest.json").read_bytes())

        descriptor = runtime["device_data"]
        assert descriptor == {
            "parser": "targets/demo/phone_data.py",
            "originals_parser": None,
            "required_groups": ["audio-profile"],
            "groups": {"audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}]},
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
            "ecc_module": None,
            "preparation_recipe": descriptor["preparation_recipe"],
        }
        assert len(descriptor["preparation_recipe"]) == 64
        assert runtime["nand"]["raw_device"] == "/dev/raw-nand"
        assert len(runtime["nand"]["acquisition_recipe"]) == 64
        parser = published / "targets/demo/phone_data.py"
        assert parser.read_bytes() == b"# declared preparation source\n"
        assert (
            runtime["sha256"]["targets/demo/phone_data.py"]
            == hashlib.sha256(parser.read_bytes()).hexdigest()
        )
        assert not (published / "device-data-manifest.json").exists()
        assert not (published / "pinmap.bin").exists()
        assert not (published / "keymap.bin").exists()

    def test_named_profile_publishes_to_its_own_current_bundle_slot(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """Publish to the named profile slot with its identity in both manifests."""
        publication_inputs.target_config["profile"] = "usb-host-lab"

        published = publication_inputs.publish()
        current = resolve_current_bundle(publication_inputs.output, "demo", "usb-host-lab")
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        runtime = json.loads((published / "runtime-manifest.json").read_text())

        assert (published.parent) == (
            publication_inputs.output / "demo/profiles/usb-host-lab/builds/release/bundles"
        )
        assert (current.path) == (published)
        assert (manifest["profile"]) == ("usb-host-lab")
        assert (runtime["profile"]) == ("usb-host-lab")
        assert (runtime["transport"]) == ("usb-ncm")

    def test_publishing_debug_retains_release_and_records_its_type_in_both_manifests(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """Publication binds the new type without moving the release current pointer."""
        release = publication_inputs.publish()
        publication_inputs.target_config["build_type"] = "debug"
        debug = publication_inputs.publish()

        assert (resolve_current_bundle(publication_inputs.output, "demo").path) == (release)
        assert (
            resolve_current_bundle(publication_inputs.output, "demo", build_type="debug").path
        ) == (debug)
        assert (release.name) != (debug.name)
        for name in ("build-manifest.json", "runtime-manifest.json"):
            manifest = json.loads((debug / name).read_text())
            assert (manifest["build_type"]) == ("debug")

    def test_external_root_keeps_the_exact_composition_input_for_host_inspection(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """The external root's local composition input is bound to the bundle manifest."""
        publication_inputs.target_config["profile"] = "microsd"
        publication_inputs.target_config["linux"]["root"] = {
            "kind": "external",
            "filesystem": "ext4",
            "partuuid": "46504c58-02",
            "wait_seconds": 10,
        }
        publication_inputs.target_config["bootstrap"]["initramfs_delivery"] = "none"

        publication_inputs.initramfs.unlink()
        published = publication_inputs.publish()

        assert not ((published / "debug/initramfs.cpio").exists())
        assert ((published / "debug/rootfs.cpio").read_bytes()) == (b"logical composition\n")
        manifest = json.loads((published / BUILD_MANIFEST_NAME).read_text())
        assert ("debug/rootfs.cpio") in (manifest["files"])

    def test_publish_rejects_apks_outside_the_declared_bundle_package_set(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """A bundle cannot silently add or omit one of its declared packages."""
        current = publication_inputs.publish()

        with pytest.raises(SystemExit, match="declared bundle package set"):
            publication_inputs.publish(bundle_packages=("fplinux-base-ui",))

        publication_inputs.assert_old_current_and_no_staging(current)

    def test_publish_rejects_release_manifest_apks_outside_the_declared_set(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """The release manifest cannot omit or add an independently named APK."""
        current = publication_inputs.publish()
        bundle_files = cast("list[str]", publication_inputs.release_manifest["bundle_files"])
        bundle_files.remove("apks/fplinux-demo-ui.apk")

        with pytest.raises(SystemExit, match="release manifest APK files"):
            publication_inputs.publish()

        publication_inputs.assert_old_current_and_no_staging(current)

    def test_copy_failure_preserves_the_current_bundle_and_cleans_staging(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """Retain the current bundle when copying a new payload fails."""
        first = publication_inputs.publish()
        create_bundle_staging(publication_inputs.output, "demo")
        missing = publication_inputs.work / "missing-ramboot.bin"

        with pytest.raises(SystemExit):
            publication_inputs.publish(missing)

        assert (resolve_current_bundle(publication_inputs.output, "demo").path) == (first)
        assert (publication_inputs.staging_directories()) == ([])

    def test_manifest_failure_preserves_the_current_bundle_and_cleans_staging(
        self, publication_inputs: PublicationInputs
    ) -> None:
        """Retain the current bundle when writing its new manifest fails."""
        first = publication_inputs.publish()
        real_write_json = publish_build.write_json
        message = "manifest write failed"

        def write_json(path: Path, value: dict[str, object]) -> None:
            if path.name == BUILD_MANIFEST_NAME:
                raise OSError(message)
            real_write_json(path, value)

        with (
            mock.patch.object(publish_build, "write_json", side_effect=write_json),
            pytest.raises(OSError, match=message),
        ):
            publication_inputs.publish()

        publication_inputs.assert_old_current_and_no_staging(first)

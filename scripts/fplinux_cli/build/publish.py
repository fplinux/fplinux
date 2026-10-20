# SPDX-License-Identifier: GPL-2.0-only
"""Publish complete immutable target bundles."""

from __future__ import annotations

import json
import os
import shutil
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine.rootfs_state import INITRAMFS_NAME, trusted_receipt_identity
from fplinux_cli.alpine.signing import signing_key_identity
from fplinux_cli.artifacts.bundles import (
    create_bundle_staging,
    discard_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
    published_file_records,
)
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build.identity import runtime_identity
from fplinux_cli.build.runtime import device_data_descriptor, runtime_source_files
from fplinux_cli.build.storage.stage import default_boot_artifacts
from fplinux_cli.common import (
    canonical_json_bytes,
    fail,
    replace_file_atomically,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.manifests.values import relative_value, sha256_value

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.bootstrap.result import PayloadRegion


def copy_file(source: Path, destination: Path, *, executable: bool = False) -> None:
    """Copy a validated output with a normalized mode."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(inputs_build.require_file(source), destination)
    destination.chmod(0o755 if executable else 0o644)


def write_json(path: Path, value: dict[str, Any]) -> None:
    """Atomically write deterministic JSON metadata."""
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()
    replace_file_atomically(path, encoded, 0o644, sync=False)


def runtime_manifest(  # noqa: PLR0913 -- each published artifact role stays explicit.
    release: Path,
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    *,
    image: str,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
    personalization: dict[str, int | str],
    payload_regions: dict[str, PayloadRegion] | None = None,
) -> dict[str, Any]:
    """Create the generic runtime contract consumed by the common runner."""
    declared_assets = {
        role: f"assets/{relative}" for role, (relative, _digest) in asset_outputs.items()
    }

    platform_host = platform["host"]
    runtime = target_config["runtime"]
    runtime_tools = {role: f"host/{name}" for role, name in platform_host["runtime_tools"].items()}
    device_data = device_data_descriptor(target, target_config)
    if device_data is not None and device_data["ecc_module"] is not None:
        from fplinux_cli.device_data.prepare import (  # noqa: PLC0415 -- selected ECC runtime.
            NAND_ECC_TOOL,
        )

        runtime_tools["nand_ecc"] = f"host/{NAND_ECC_TOOL}"
    for relative in runtime_tools.values():
        name = relative.removeprefix("host/")
        if name not in host_tools:
            fail(f"runtime host tool was not built: {name}")
    hashes = {
        image: sha256_file(inputs_build.require_file(release / image)),
        **{
            relative: sha256_file(inputs_build.require_file(release / relative))
            for relative in runtime_source_files(target, target_config)
        },
    }
    hashes.update(
        {
            declared_assets[role]: sha256_file(
                inputs_build.require_file(release / declared_assets[role])
            )
            for role in asset_outputs
        }
    )
    hashes.update(
        {
            runtime_tools[role]: sha256_file(
                inputs_build.require_file(release / runtime_tools[role])
            )
            for role in runtime_tools
        }
    )
    adapter = dict(runtime["adapter"])
    if payload_regions is not None:
        adapter["regions"] = payload_regions
    return {
        "target": target,
        "profile": inputs_build.selected_profile(target_config),
        "build_type": target_config["build_type"],
        "transport": runtime.get("transport", "usb-ncm"),
        "identity": runtime_identity(
            target_config["identity"],
            target_config["platform"],
            platform["identity"],
        ),
        "image": image,
        "addresses": {
            "fdl1": runtime["fdl1_load_address"],
            "payload": target_config["bootstrap"]["load_address"],
        },
        "usb": runtime["usb"],
        "personalization": personalization,
        "assets": declared_assets,
        "adapter": adapter,
        "host_tools": runtime_tools,
        "nand": target_config.get("nand"),
        "device_data": device_data,
        "sha256": hashes,
    }


def _publish_staged_bundle(  # noqa: PLR0913 -- artifact and receipt roles stay explicit.
    release: Path,
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    *,
    release_manifest: dict[str, Any],
    work: Path,
    rootfs: Path,
    kernel_output: Path,
    zimage: Path,
    dtb: Path,
    ramboot: Path,
    ramboot_map: Path,
    personalization: dict[str, int | str],
    asset_lock_path: Path,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
    linux_recipe: str,
    device_identity: str,
    rootfs_output: Path,
    rootfs_recipe: str,
    kbuild_receipt: dict[str, str],
    bundle_packages: tuple[str, ...],
    bundle_apks: dict[str, Path],
    boot_files: dict[str, Path],
    boot_artifacts: dict[str, Any],
    payload_regions: dict[str, PayloadRegion] | None,
) -> Path:
    """Complete one already-private immutable bundle staging directory."""
    if set(bundle_packages) != set(bundle_apks):
        fail("published bundle APKs differ from the declared bundle package set")
    expected_apk_files = {f"apks/{package}.apk" for package in bundle_packages}
    manifest_apk_files = {
        relative
        for relative in release_manifest["bundle_files"]
        if relative.startswith("apks/") and relative.endswith(".apk")
    }
    if manifest_apk_files != expected_apk_files:
        fail("release manifest APK files differ from the declared bundle package set")

    image_name = release_manifest["image"]
    copy_file(ramboot, release / image_name)
    if target_config["linux"]["root"]["kind"] == "initramfs":
        copy_file(work / "device-data.cpio", release / "device-data.cpio")
    kernel_name = zimage.name
    debug_outputs = [
        (zimage, kernel_name),
        (dtb, target_config["linux"]["debug_dtb"]),
        (kernel_output / "vmlinux", "vmlinux"),
        (kernel_output / "System.map", "System.map"),
        (kernel_output / ".config", "kernel.config"),
        (ramboot_map, "ramboot.map"),
        (rootfs, "rootfs.cpio"),
    ]
    if target_config["linux"]["root"]["kind"] == "initramfs":
        debug_outputs.append((rootfs_output / INITRAMFS_NAME, "initramfs.cpio"))
    for source, name in debug_outputs:
        copy_file(source, release / "debug" / name)
    for relative, _digest in asset_outputs.values():
        copy_file(work / "assets" / relative, release / "assets" / relative)
    for name, source in host_tools.items():
        copy_file(source, release / "host" / name, executable=True)
    for relative, source in runtime_source_files(target, target_config).items():
        copy_file(source, release / relative, executable=relative in {"fplinux.py", "Run.sh"})
    copy_file(asset_lock_path, release / "assets.lock.toml")
    copy_file(common.ROOT / "THIRD_PARTY_NOTICES.md", release / "THIRD_PARTY_NOTICES.md")
    for package, source in sorted(bundle_apks.items()):
        copy_file(source, release / "apks" / f"{package}.apk")
    for relative, source in sorted(boot_files.items()):
        destination = release / relative_value(relative, "boot artifact path")
        if destination.exists() or destination.is_symlink():
            fail(f"boot artifact collides with a bundle file: {relative}")
        copy_file(source, destination)

    runtime = runtime_manifest(
        release,
        target,
        target_config,
        platform,
        image=image_name,
        asset_outputs=asset_outputs,
        host_tools=host_tools,
        personalization=personalization,
        payload_regions=payload_regions,
    )
    write_json(release / "runtime-manifest.json", runtime)

    for relative in release_manifest["bundle_files"]:
        inputs_build.require_file(release / relative)

    workspace_digest = os.environ.get("FPLINUX_WORKSPACE_DIGEST", "")
    container_image_recipe, container_image_content = inputs_build.container_image_environment()
    sha256_value(workspace_digest, "workspace digest")
    sha256_value(linux_recipe, "Linux recipe")
    sha256_value(device_identity, "device identity")
    apk_signing_key = sha256_value(
        signing_key_identity(inputs_build.CACHE), "APK signing public key"
    )
    rootfs_receipt = trusted_receipt_identity(rootfs_output, rootfs_recipe)
    if not isinstance(kbuild_receipt, dict) or set(kbuild_receipt) != {"recipe", "sha256"}:
        fail("Kbuild receipt identity is invalid")
    kbuild_receipt = {
        "recipe": sha256_value(kbuild_receipt.get("recipe"), "Kbuild recipe"),
        "sha256": sha256_value(kbuild_receipt.get("sha256"), "Kbuild receipt SHA-256"),
    }
    payload = {
        "target": target,
        "profile": inputs_build.selected_profile(target_config),
        "build_type": target_config["build_type"],
        "workspace_digest": workspace_digest,
        "container_image_recipe": container_image_recipe,
        "container_image_content": container_image_content,
        "apk_signing_key": apk_signing_key,
        "linux_recipe": linux_recipe,
        "device_identity": device_identity,
        "rootfs_receipt": rootfs_receipt,
        "kbuild_receipt": kbuild_receipt,
        "boot_artifacts": boot_artifacts,
        "kernel_artifacts": {
            "image": f"debug/{kernel_name}",
            "config": "debug/kernel.config",
            "vmlinux": "debug/vmlinux",
            "system_map": "debug/System.map",
            "rootfs": "debug/rootfs.cpio",
            "initramfs": (
                "debug/initramfs.cpio"
                if target_config["bootstrap"]["initramfs_delivery"] != "none"
                else None
            ),
            "initramfs_delivery": target_config["bootstrap"]["initramfs_delivery"],
        },
        "files": published_file_records(release),
    }
    generation = sha256_bytes(canonical_json_bytes(payload))
    manifest = {**payload, "generation": generation}
    write_json(release / "build-manifest.json", manifest)
    profile = inputs_build.selected_profile(target_config)
    generation_path = publish_bundle_generation(
        inputs_build.OUTPUT,
        target,
        release,
        generation,
        profile,
        build_type=target_config["build_type"],
    )
    publish_current_bundle(
        inputs_build.OUTPUT,
        target,
        generation_path,
        profile,
        build_type=target_config["build_type"],
    )
    return generation_path


def publish_bundle(  # noqa: PLR0913 -- artifact and receipt roles stay explicit.
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    *,
    release_manifest: dict[str, Any],
    work: Path,
    rootfs: Path,
    kernel_output: Path,
    zimage: Path,
    dtb: Path,
    ramboot: Path,
    ramboot_map: Path,
    personalization: dict[str, int | str],
    asset_lock_path: Path,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
    linux_recipe: str,
    device_identity: str,
    rootfs_output: Path,
    rootfs_recipe: str,
    kbuild_receipt: dict[str, str],
    bundle_packages: tuple[str, ...],
    bundle_apks: dict[str, Path],
    boot_files: dict[str, Path] | None = None,
    boot_artifacts: dict[str, Any] | None = None,
    payload_regions: dict[str, PayloadRegion] | None = None,
) -> Path:
    """Publish a complete immutable bundle and select it as current."""
    profile = inputs_build.selected_profile(target_config)
    release = create_bundle_staging(
        inputs_build.OUTPUT, target, profile, build_type=target_config["build_type"]
    )
    if boot_files is None:
        boot_files = {}
    if boot_artifacts is None:
        boot_artifacts = default_boot_artifacts()
    try:
        return _publish_staged_bundle(
            release,
            target,
            target_config,
            platform,
            release_manifest=release_manifest,
            work=work,
            rootfs=rootfs,
            kernel_output=kernel_output,
            zimage=zimage,
            dtb=dtb,
            ramboot=ramboot,
            ramboot_map=ramboot_map,
            personalization=personalization,
            asset_lock_path=asset_lock_path,
            asset_outputs=asset_outputs,
            host_tools=host_tools,
            linux_recipe=linux_recipe,
            device_identity=device_identity,
            rootfs_output=rootfs_output,
            rootfs_recipe=rootfs_recipe,
            kbuild_receipt=kbuild_receipt,
            bundle_packages=bundle_packages,
            bundle_apks=bundle_apks,
            boot_files=boot_files,
            boot_artifacts=boot_artifacts,
            payload_regions=payload_regions,
        )
    finally:
        discard_bundle_staging(
            inputs_build.OUTPUT, target, release, profile, build_type=target_config["build_type"]
        )

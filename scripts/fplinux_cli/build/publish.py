# SPDX-License-Identifier: GPL-2.0-only
"""Publish complete immutable target bundles."""

from __future__ import annotations

import json
import os
import shutil
from typing import TYPE_CHECKING, Any

from fplinux_cli import alpine_state, common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import storage as storage_build
from fplinux_cli.bundle_state import (
    create_bundle_staging,
    discard_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
    published_file_records,
)
from fplinux_cli.common import (
    canonical_json_bytes,
    fail,
    replace_file_atomically,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.identity import RUNTIME_IDENTITY_PATH
from fplinux_cli.identity_codegen import runtime_identity
from fplinux_cli.manifests.values import relative_value

if TYPE_CHECKING:
    from pathlib import Path


def runner_source() -> Path:
    """Return the one shared runner source path."""
    return common.ROOT / "common/run.py"


def ssh_transport_source() -> Path:
    """Return the SSH session helper published beside the runner."""
    return common.ROOT / "scripts/fplinux_cli/ssh_transport.py"


def identity_source() -> Path:
    """Return the shared identity contract published beside the runner."""
    return common.ROOT / "scripts/fplinux_cli/identity.py"


def adapter_source(platform: str) -> Path:
    """Return the fixed adapter path for a validated platform."""
    return common.ROOT / "platforms" / platform / "host/adapter.py"


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
) -> dict[str, Any]:
    """Create the generic runtime contract consumed by the common runner."""
    declared_assets = {
        role: f"assets/{relative}" for role, (relative, _digest) in asset_outputs.items()
    }

    platform_host = platform["host"]
    runtime = target_config["runtime"]
    runtime_tools = {role: f"host/{name}" for role, name in platform_host["runtime_tools"].items()}
    for name in platform_host["runtime_tools"].values():
        if name not in host_tools:
            fail(f"runtime host tool was not built: {name}")
    hashes = {
        image: sha256_file(inputs_build.require_file(release / image)),
        "runner/platform_adapter.py": sha256_file(
            inputs_build.require_file(release / "runner/platform_adapter.py")
        ),
    }
    hashes["runner/ssh_transport.py"] = sha256_file(
        inputs_build.require_file(release / "runner/ssh_transport.py")
    )
    hashes[RUNTIME_IDENTITY_PATH] = sha256_file(
        inputs_build.require_file(release / RUNTIME_IDENTITY_PATH)
    )
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
            for role in platform_host["runtime_tools"]
        }
    )
    return {
        "target": target,
        "profile": inputs_build.selected_profile(target_config),
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
        "adapter": runtime["adapter"],
        "host_tools": runtime_tools,
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
    debug_outputs = [
        (zimage, "zImage"),
        (dtb, target_config["linux"]["debug_dtb"]),
        (kernel_output / "vmlinux", "vmlinux"),
        (kernel_output / "System.map", "System.map"),
        (kernel_output / ".config", "kernel.config"),
        (ramboot_map, "ramboot.map"),
    ]
    if target_config["linux"]["root"]["kind"] == "initramfs":
        debug_outputs.append((rootfs, "rootfs.cpio"))
    for source, name in debug_outputs:
        copy_file(source, release / "debug" / name)
    for relative, _digest in asset_outputs.values():
        copy_file(work / "assets" / relative, release / "assets" / relative)
    for name, source in host_tools.items():
        copy_file(source, release / "host" / name, executable=True)
    copy_file(runner_source(), release / "runner/run.py", executable=True)
    copy_file(ssh_transport_source(), release / "runner/ssh_transport.py")
    copy_file(identity_source(), release / RUNTIME_IDENTITY_PATH)
    copy_file(
        adapter_source(target_config["platform"]),
        release / "runner/platform_adapter.py",
    )
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
    )
    write_json(release / "runtime-manifest.json", runtime)

    for relative in release_manifest["bundle_files"]:
        inputs_build.require_file(release / relative)

    workspace_digest = os.environ.get("FPLINUX_WORKSPACE_DIGEST", "")
    container_image_recipe, container_image_generation = inputs_build.container_image_environment()
    inputs_build.require_sha256(workspace_digest, "workspace digest")
    inputs_build.require_sha256(linux_recipe, "Linux recipe")
    inputs_build.require_sha256(device_identity, "device identity")
    apk_signing_key = inputs_build.require_sha256(
        alpine_state.signing_key_identity(inputs_build.CACHE), "APK signing public key"
    )
    rootfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
    if not isinstance(kbuild_receipt, dict) or set(kbuild_receipt) != {"recipe", "sha256"}:
        fail("Kbuild receipt identity is invalid")
    kbuild_receipt = {
        "recipe": inputs_build.require_sha256(kbuild_receipt.get("recipe"), "Kbuild recipe"),
        "sha256": inputs_build.require_sha256(
            kbuild_receipt.get("sha256"), "Kbuild receipt SHA-256"
        ),
    }
    payload = {
        "target": target,
        "profile": inputs_build.selected_profile(target_config),
        "workspace_digest": workspace_digest,
        "container_image_recipe": container_image_recipe,
        "container_image_generation": container_image_generation,
        "apk_signing_key": apk_signing_key,
        "linux_recipe": linux_recipe,
        "device_identity": device_identity,
        "rootfs_receipt": rootfs_receipt,
        "kbuild_receipt": kbuild_receipt,
        "boot_artifacts": boot_artifacts,
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
    )
    publish_current_bundle(inputs_build.OUTPUT, target, generation_path, profile)
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
) -> Path:
    """Publish a complete immutable bundle and select it as current."""
    profile = inputs_build.selected_profile(target_config)
    release = create_bundle_staging(inputs_build.OUTPUT, target, profile)
    if boot_files is None:
        boot_files = {}
    if boot_artifacts is None:
        boot_artifacts = storage_build.default_boot_artifacts()
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
        )
    finally:
        discard_bundle_staging(inputs_build.OUTPUT, target, release, profile)

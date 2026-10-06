# SPDX-License-Identifier: GPL-2.0-only
"""Run the ordered target build and publication stages."""

from __future__ import annotations

import argparse
import os

from fplinux_cli import common
from fplinux_cli.alpine.rootfs import build_rootfs
from fplinux_cli.alpine.selection import bundle_packages, selected_packages
from fplinux_cli.artifacts.bundles import bundle_slot
from fplinux_cli.build import assets as assets_build
from fplinux_cli.build import host as host_build
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import publish as publish_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.bootstrap.recipe import bootstrap_recipe_digest
from fplinux_cli.build.bootstrap.stage import build_bootstrap
from fplinux_cli.build.kernel.compile import build_kernel
from fplinux_cli.build.kernel.prepare import prepare_linux
from fplinux_cli.build.storage import stage as storage_build
from fplinux_cli.common import fail
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.environment.images import container_artifact_recipe_digest
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.paths import target_asset_lock_path
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.releases import load_release
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.reporting.run import RunReporter, run_entrypoint


def main() -> None:
    """Build stages 1-4 and publish one deterministic target bundle."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--profile")
    parser.add_argument("--build-type", choices=BUILD_TYPES, default="release")
    parser.add_argument("--jobs", type=int, required=True)
    args = parser.parse_args()
    if args.jobs < 1:
        fail("jobs must be positive")
    process_build.lower_build_priority()

    image_recipe, image_content = inputs_build.container_image_environment()
    os.environ["FPLINUX_CONTAINER_IMAGE_RECIPE"] = container_artifact_recipe_digest(
        image_recipe, image_content
    )

    reporter = RunReporter.from_environment(f"build {args.target}", "build")
    with process_build.report_stage(reporter, "configuration"):
        target_config = load_target(args.target, args.profile, build_type=args.build_type)
        platform = load_platform(target_config["platform"])
        rootfs_packages = selected_packages(platform, target_config)
        selected_bundle_packages = bundle_packages(platform, target_config, rootfs_packages)
        device_data = firmware_inputs.capture_snapshot_device_data(
            args.target,
            target_config["device_data"]["groups"],
            common.ROOT,
        )
        firmware = firmware_inputs.rootfs_firmware_inputs(device_data)
        audio_profile_firmware = device_data.get("audio-profile")
        sources = common.load_toml(common.ROOT / "sources.lock.toml")
        linux_base = inputs_build.require_sha256(
            sources_build.source_lock_entry(sources, platform["linux"]["source_lock"]).get(
                "sha256"
            ),
            "Linux source",
        )
        asset_lock_path = inputs_build.require_file(target_asset_lock_path(args.target))
        release_manifest = load_release(
            args.target, profile_packages=target_config["rootfs"]["packages"]
        )

        profile = inputs_build.selected_profile(target_config)
        work = (
            bundle_slot(inputs_build.OUTPUT, args.target, profile, build_type=args.build_type)
            / "work"
        )
        work.mkdir(parents=True, exist_ok=True)
        inputs_build.CACHE.mkdir(parents=True, exist_ok=True)

    with process_build.report_stage(reporter, "prepare-linux"):
        linux_source, prepared_linux = prepare_linux(
            sources,
            args.target,
            target_config,
            platform,
        )

    with process_build.report_stage(reporter, "rootfs"):
        if target_config["image"]["kind"] == "ext4-root":
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = build_rootfs(
                args.jobs,
                rootfs_packages,
                selected_bundle_packages,
                firmware=firmware,
                display_brightness=target_config.get("display_brightness"),
                external_image=target_config["image"],
                external_output=work / "rootfs-image",
            )
        else:
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = build_rootfs(
                args.jobs,
                rootfs_packages,
                selected_bundle_packages,
                firmware=firmware,
                display_brightness=target_config.get("display_brightness"),
            )
        ext4_artifact = storage_build.profile_ext4_artifact(
            target_config,
            work,
            rootfs_output,
            rootfs_recipe,
        )
    cross = platform["linux"]["cross_compile"]
    kernel_output = work / "kernel"
    with process_build.report_stage(reporter, "kernel"):
        bootstrap_recipe = bootstrap_recipe_digest(
            sources,
            args.target,
            target_config,
            platform,
        )
        zimage, dtb, kbuild_receipt, device_identity = build_kernel(
            args.target,
            target_config,
            platform,
            bootstrap_recipe=bootstrap_recipe,
            linux_source=linux_source,
            prepared_linux=prepared_linux,
            linux_base=linux_base,
            output=kernel_output,
            cross=cross,
            rootfs=rootfs,
            rootfs_output=rootfs_output,
            rootfs_recipe=rootfs_recipe,
            audio_profile_firmware=audio_profile_firmware,
            jobs=args.jobs,
        )
    profile_uboot = None
    if target_config["uboot"]["kind"] != "none":
        with process_build.report_stage(reporter, "uboot"):
            profile_uboot = storage_build.build_profile_uboot(
                args.target, target_config, work, args.jobs
            )
    fit_artifact = None
    if target_config["fit"]["kind"] != "none":
        with process_build.report_stage(reporter, "fit"):
            fit_artifact = storage_build.build_profile_fit(
                args.target,
                target_config,
                work=work,
                zimage=zimage,
                dtb=dtb,
                uboot=profile_uboot,
            )
    sd_image_artifact = None
    if target_config["image"]["kind"] != "none":
        with process_build.report_stage(reporter, "sd-image"):
            sd_image_artifact = storage_build.build_profile_sd_image(
                target_config,
                work,
                fit_artifact,
                ext4_artifact,
            )
    boot_files, boot_artifacts = storage_build.profile_boot_artifact_set(
        target_config,
        sd_image_artifact,
    )
    with process_build.report_stage(reporter, "bootstrap"):
        ramboot, ramboot_map, personalization = build_bootstrap(
            sources,
            args.target,
            target_config,
            platform,
            work=work,
            zimage=zimage,
            dtb=dtb,
            uboot=profile_uboot,
        )
    with process_build.report_stage(reporter, "assets"):
        asset_outputs = assets_build.build_assets(asset_lock_path, work / "assets")
    with process_build.report_stage(reporter, "host-tools"):
        host_tools = host_build.build_host_tools(sources, platform, work)
    with process_build.report_stage(reporter, "publish"):
        publish_build.publish_bundle(
            args.target,
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
            linux_recipe=prepared_linux.linux_recipe,
            device_identity=device_identity,
            rootfs_output=rootfs_output,
            rootfs_recipe=rootfs_recipe,
            kbuild_receipt=kbuild_receipt,
            bundle_packages=selected_bundle_packages,
            bundle_apks=bundle_apk_outputs,
            boot_files=boot_files,
            boot_artifacts=boot_artifacts,
        )


if __name__ == "__main__":
    run_entrypoint(main)

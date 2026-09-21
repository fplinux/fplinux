# SPDX-License-Identifier: GPL-2.0-only
"""Run the ordered target build and publication stages."""

from __future__ import annotations

import argparse
import os

from fplinux_cli import alpine_builder, alpine_state, common, firmware_inputs
from fplinux_cli.build import assets as assets_build
from fplinux_cli.build import bootstrap as bootstrap_build
from fplinux_cli.build import host as host_build
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import kernel as kernel_build
from fplinux_cli.build import linux as linux_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import publish as publish_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build import storage as storage_build
from fplinux_cli.common import fail
from fplinux_cli.environment.images import container_runtime_recipe_digest
from fplinux_cli.manifests.paths import target_asset_lock_path
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.releases import load_release
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.output import RunReporter, run_entrypoint


def main() -> None:
    """Build stages 1-4 and publish one deterministic target bundle."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--profile")
    parser.add_argument("--jobs", type=int, required=True)
    args = parser.parse_args()
    if args.jobs < 1:
        fail("jobs must be positive")

    image_recipe, image_generation = inputs_build.container_image_environment()
    os.environ["FPLINUX_CONTAINER_IMAGE_RECIPE"] = container_runtime_recipe_digest(
        image_recipe, image_generation
    )

    reporter = RunReporter.from_environment(f"build {args.target}", "build")
    with process_build.report_stage(reporter, "configuration"):
        target_config = load_target(args.target, args.profile)
        platform = load_platform(target_config["platform"])
        rootfs_packages = alpine_state.selected_packages(platform, target_config)
        bundle_packages = alpine_state.bundle_packages(platform, target_config, rootfs_packages)
        device_data = firmware_inputs.capture_snapshot_device_data(
            args.target,
            target_config["device_data"]["groups"],
            common.ROOT,
        )
        firmware = device_data.get("bluetooth", ())
        audio_profile_firmware = device_data.get("audio-profile")
        sources = common.load_toml(common.ROOT / "sources.lock.toml")
        linux_base = inputs_build.require_sha256(
            sources_build.source_lock_entry(sources, platform["linux"]["source_lock"]).get(
                "sha256"
            ),
            "Linux source",
        )
        asset_lock_path = inputs_build.require_file(target_asset_lock_path(args.target))
        release_manifest = load_release(args.target)

        profile = inputs_build.selected_profile(target_config)
        work = inputs_build.OUTPUT / args.target / "work"
        if profile is not None:
            work = inputs_build.OUTPUT / args.target / "profiles" / profile / "work"
        work.mkdir(parents=True, exist_ok=True)
        inputs_build.CACHE.mkdir(parents=True, exist_ok=True)

    with process_build.report_stage(reporter, "prepare-linux"):
        linux_source, prepared_linux = linux_build.prepare_linux(
            sources,
            args.target,
            target_config,
            platform,
        )

    with process_build.report_stage(reporter, "rootfs"):
        if target_config["image"]["kind"] == "ext4-root":
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = alpine_builder.build_rootfs(
                args.jobs,
                rootfs_packages,
                bundle_packages,
                firmware=firmware,
                external_image=target_config["image"],
                external_output=work / "rootfs-image",
            )
        else:
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = alpine_builder.build_rootfs(
                args.jobs,
                rootfs_packages,
                bundle_packages,
                firmware=firmware,
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
        bootstrap_recipe = bootstrap_build.bootstrap_recipe_digest(
            sources,
            args.target,
            target_config,
            platform,
        )
        zimage, dtb, kbuild_receipt, device_identity = kernel_build.build_kernel(
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
        ramboot, ramboot_map, personalization = bootstrap_build.build_bootstrap(
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
            bundle_packages=bundle_packages,
            bundle_apks=bundle_apk_outputs,
            boot_files=boot_files,
            boot_artifacts=boot_artifacts,
        )


if __name__ == "__main__":
    run_entrypoint(main)

# SPDX-License-Identifier: GPL-2.0-only
"""Run the ordered target build and publication stages."""

from __future__ import annotations

import argparse
import os

from fplinux_cli import common
from fplinux_cli.alpine.lock import bundle_runtime_packages, load_alpine_lock
from fplinux_cli.alpine.rootfs import (
    build_apk_catalogue,
    build_rootfs,
    shared_card_packages,
    shared_ram_packages,
)
from fplinux_cli.alpine.rootfs_files import write_card_initramfs
from fplinux_cli.alpine.rootfs_state import EXT4_IMAGE_DIRECTORY
from fplinux_cli.alpine.selection import (
    bundle_packages,
    selected_applications,
    selected_base_packages,
)
from fplinux_cli.artifacts.bundles import bundle_slot
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.build import assets as assets_build
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import publish as publish_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.bootstrap.recipe import bootstrap_recipe_digest
from fplinux_cli.build.bootstrap.stage import build_bootstrap
from fplinux_cli.build.host_tools import host as host_build
from fplinux_cli.build.kernel.compile import build_kernel
from fplinux_cli.build.kernel.prepare import prepare_linux
from fplinux_cli.build.runtime import runtime_host_tools, runtime_needs_nand_ecc
from fplinux_cli.build.storage import stage as storage_build
from fplinux_cli.common import fail
from fplinux_cli.device_data import inputs as firmware_inputs
from fplinux_cli.device_data.cpio import create_device_data_initramfs
from fplinux_cli.environment.definition import environment_artifact_recipe_digest
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.releases import load_release
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.manifests.values import sha256_value
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

    environment_recipe, environment_content = inputs_build.environment_identity()
    os.environ["FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE"] = environment_artifact_recipe_digest(
        environment_recipe, environment_content
    )

    reporter = RunReporter.from_environment(f"build {args.target}", "build")
    with process_build.report_stage(reporter, "configuration"):
        target_config = load_target(args.target, args.profile, build_type=args.build_type)
        platform = load_platform(target_config["platform"])
        rootfs_packages = selected_base_packages(platform, target_config)
        applications = selected_applications(target_config)
        selected_bundle_packages = bundle_packages(platform, target_config, rootfs_packages)
        bundle_apk_packages = (
            *selected_bundle_packages,
            *bundle_runtime_packages(
                load_alpine_lock(), rootfs_packages, selected_bundle_packages
            ),
        )
        device_data = firmware_inputs.capture_snapshot_device_data(
            args.target,
            target_config["device_data"]["groups"],
            common.ROOT,
        )
        card_packages = None
        if (common.ROOT / ".fplinux-release-card").is_file():
            card_packages = shared_card_packages(platform, target_config)
            if card_packages is None or device_data:
                fail("release card build requires a supported profile without device data")
        external_root = target_config["linux"]["root"]["kind"] == "external"
        slotted_card = target_config["profile"] == "microsd-uboot"
        rootfs_delivery = firmware_inputs.rootfs_receives_firmware(
            root_kind=target_config["linux"]["root"]["kind"],
        )
        firmware = firmware_inputs.rootfs_firmware_inputs(device_data) if rootfs_delivery else ()
        sources = common.load_toml(common.ROOT / "sources.lock.toml")
        linux_base = sha256_value(
            sources_build.source_lock_entry(sources, platform["linux"]["source_lock"]).get(
                "sha256"
            ),
            "Linux source",
        )
        release_manifest = load_release(args.target, bundle_packages=bundle_apk_packages)

        profile = inputs_build.selected_profile(target_config)
        work = (
            bundle_slot(inputs_build.OUTPUT, args.target, profile, build_type=args.build_type)
            / "work"
        )
        work.mkdir(parents=True, exist_ok=True)
        if not external_root:
            device_data_initramfs = work / "device-data.cpio"
            capsule_groups = {} if rootfs_delivery else device_data
            device_data_initramfs.write_bytes(create_device_data_initramfs(capsule_groups))
            device_data_initramfs.chmod(0o600)
        inputs_build.CACHE.mkdir(parents=True, exist_ok=True)

    with process_build.report_stage(reporter, "prepare-linux"):
        linux_source, prepared_linux = prepare_linux(
            sources,
            args.target,
            target_config,
            platform,
        )

    card_rootfs = None
    card_recipe = None
    card_board_rootfs = None
    ext4_artifact = None
    early_firmware = None
    fit_ramdisk = None
    with process_build.report_stage(reporter, "rootfs"):
        if target_config["image"]["kind"] == "ext4-root":
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = build_rootfs(
                args.jobs,
                rootfs_packages,
                selected_bundle_packages,
                applications=applications,
                firmware=firmware,
                display_brightness=target_config.get("display_brightness"),
                root_kind="external",
                external_image=None if slotted_card else target_config["image"],
                external_output=None if slotted_card else work / "rootfs-image",
            )
        else:
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = build_rootfs(
                args.jobs,
                rootfs_packages,
                selected_bundle_packages,
                applications=applications,
                firmware=firmware,
                display_brightness=target_config.get("display_brightness"),
                ram_base_packages=shared_ram_packages(platform, target_config),
            )
        apk_catalogue = build_apk_catalogue(rootfs, bundle_apk_outputs, work / "apks")
        if not slotted_card:
            ext4_artifact = storage_build.profile_ext4_artifact(
                target_config,
                work / "rootfs-image",
                rootfs_output,
                rootfs_recipe,
            )
        if card_packages is not None:
            card_rootfs, _, card_recipe, _ = build_rootfs(
                args.jobs, card_packages, root_kind="external"
            )
            card_board_rootfs, _, _, _ = build_rootfs(
                args.jobs,
                rootfs_packages,
                display_brightness=target_config.get("display_brightness"),
                root_kind="external",
            )
        if external_root:
            early_payload = create_device_data_initramfs(
                {"audio-profile": device_data.get("audio-profile", ())}
            )
            if slotted_card:
                fit_ramdisk = work / "fit-initramfs.cpio"
                write_card_initramfs(
                    rootfs,
                    fit_ramdisk,
                    target=args.target,
                    config=target_config,
                    build_id=current_release_context().release_id,
                    audio=early_payload,
                )
            else:
                early_firmware = work / "early-firmware.cpio"
                early_firmware.write_bytes(early_payload)
                early_firmware.chmod(0o600)
    cross = platform["linux"]["cross_compile"]
    kernel_output = (
        inputs_build.CACHE
        / "linux/builds"
        / target_config["platform"]
        / args.build_type
        / "kernel"
    )
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
            board_output=work,
            cross=cross,
            rootfs_output=rootfs_output,
            rootfs_recipe=rootfs_recipe,
            early_firmware=fit_ramdisk if fit_ramdisk is not None else early_firmware,
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
                ramdisk=fit_ramdisk,
                uboot=profile_uboot,
            )
    with process_build.report_stage(reporter, "bootstrap"):
        bootstrap = build_bootstrap(
            sources,
            args.target,
            target_config,
            platform,
            work=work,
            zimage=zimage,
            dtb=dtb,
            uboot=profile_uboot,
            kernel_output=kernel_output,
            rootfs_output=rootfs_output,
            early_firmware=early_firmware,
        )
    sd_image_artifact = None
    if target_config["image"]["kind"] != "none":
        with process_build.report_stage(reporter, "sd-image"):
            if slotted_card:
                from fplinux_cli.build.storage import card as card_build  # noqa: PLC0415

                if fit_artifact is None:
                    fail("system card image requires a verified FIT")
                ext4_artifact = card_build.build_partition(
                    args.target,
                    target_config["image"],
                    rootfs,
                    rootfs_recipe,
                    work / "rootfs-image",
                    fit=fit_artifact,
                )
            sd_image_artifact = storage_build.build_profile_sd_image(
                target_config,
                work,
                ext4_artifact,
                cold_boot=bootstrap.cold_sd,
            )
    boot_files, boot_artifacts = storage_build.profile_boot_artifact_set(
        target_config,
        sd_image_artifact,
    )
    boot_files.update(apk_catalogue)
    if card_rootfs is not None:
        from fplinux_cli.build.storage import card as card_build  # noqa: PLC0415

        with process_build.report_stage(reporter, "card-install-assets"):
            if card_recipe is None or card_board_rootfs is None:
                fail("release card requires the shared and board system roots")
            card_ext4 = card_build.build_partition(
                args.target,
                target_config["image"],
                card_rootfs,
                card_recipe,
                card_rootfs.parent / EXT4_IMAGE_DIRECTORY,
                fit=None,
            )
            boot_files.update(
                card_build.build(
                    args.target,
                    target_config,
                    work / "card",
                    board_rootfs=card_board_rootfs,
                    shared_rootfs=card_rootfs,
                    shared_ext4=card_ext4,
                    fit=fit_artifact,
                    ramdisk=fit_ramdisk,
                    cold_boot=bootstrap.cold_sd,
                    bundle_apks=bundle_apk_outputs,
                    apk_catalogue=apk_catalogue,
                    applications=applications,
                )
            )
    with process_build.report_stage(reporter, "assets"):
        asset_lock_path = work / "assets.lock.toml"
        if target_config["bootstrap"]["backend"] == "sc9820e":
            from fplinux_cli.build.loaders import build_loaders  # noqa: PLC0415

            asset_outputs = build_loaders(
                sources,
                args.target,
                target_config,
                work=work,
                jobs=args.jobs,
                lock_output=asset_lock_path,
            )
        else:
            asset_outputs = assets_build.build_assets(
                target_config["loader_assets"], work / "assets", lock_output=asset_lock_path
            )
    with process_build.report_stage(reporter, "host-tools"):
        required_tools = runtime_host_tools(
            platform,
            nand_ecc=runtime_needs_nand_ecc(target_config),
        )
        host_tools = host_build.build_host_tools(
            sources, platform, work, tools=required_tools.values()
        )
    with process_build.report_stage(reporter, "publish"):
        publish_build.publish_bundle(
            args.target,
            target_config,
            platform,
            release_manifest=release_manifest,
            work=work,
            rootfs=rootfs,
            linux_source=linux_source,
            kernel_output=kernel_output,
            zimage=zimage,
            dtb=bootstrap.dtb or dtb,
            ramboot=bootstrap.image,
            ramboot_map=bootstrap.map,
            personalization=bootstrap.personalization,
            payload_regions=bootstrap.regions,
            asset_lock_path=asset_lock_path,
            asset_outputs=asset_outputs,
            host_tools=host_tools,
            linux_recipe=prepared_linux.linux_recipe,
            device_identity=device_identity,
            rootfs_output=rootfs_output,
            rootfs_recipe=rootfs_recipe,
            kbuild_receipt=kbuild_receipt,
            bundle_packages=bundle_apk_packages,
            bundle_apks=bundle_apk_outputs,
            boot_files=boot_files,
            boot_artifacts=boot_artifacts,
        )


if __name__ == "__main__":
    run_entrypoint(main)

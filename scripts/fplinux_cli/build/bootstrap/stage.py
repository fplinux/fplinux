# SPDX-License-Identifier: GPL-2.0-only
"""Prepare, compile and verify the selected pre-Linux boot payload."""

from __future__ import annotations

import shutil
import subprocess
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine.rootfs_state import INITRAMFS_NAME
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.display import render_bootstrap_display
from fplinux_cli.build.storage.layout import ZIMAGE_WORKSPACE_BYTES
from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import acquire_source

from .recipe import (
    bootstrap_display,
    effective_boot_layout,
    generated_bootstrap_files,
    uboot_build_header,
)
from .result import BootstrapBuild, ColdSdBootstrap
from .verify import verify_images, verify_sd_stage0_image

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.storage.uboot import UbootBuild


def _compile_stage(
    source: Path, platform: dict[str, Any], target: dict[str, Any], *, entry: str
) -> None:
    entry_argument = f"FPLINUX_BOOTSTRAP_ENTRY={entry}"
    process_build.run(["make", "-C", str(source), platform["safety_target"], entry_argument])
    process_build.run(
        [
            "make",
            "-C",
            str(source),
            *platform["build_targets"],
            f"TOOLCHAIN={target['toolchain']}",
            f"LTO={target['lto']}",
            entry_argument,
        ]
    )


def _build_card_reader(archive: Path, prefix: str, files: list[str], directory: Path) -> Path:
    """Build the pinned read-only first stage without modifying the resident bootstrap."""
    extras = [
        "sdboot_t117/Makefile",
        "sdboot_t117/entry.c",
        "sdboot_t117/main.c",
        "sdboot_t117/start.s",
        "fpdoom/fatfile.c",
        "fpdoom/fatfile.h",
        "fpdoom/microfat.c",
        "fpdoom/microfat.h",
        "fpdoom/sdio.h",
        "ums9117/init_t117.h",
        "ums9117/lcd_config.h",
        "ums9117/sdio.c",
        "ums9117/sdram_init.c",
        "ums9117/ums9117_fdl1.ld",
    ]
    sources_build.extract_vendor(archive, prefix, list(dict.fromkeys(files + extras)), directory)
    reader = directory / "sdboot_t117"
    process_build.run(
        [
            "make",
            "-C",
            str(reader),
            "TOOLCHAIN=arm-none-eabi",
            "OBJDIR=obj",
            "compile_asm=$(CC) -mcpu=cortex-a7 -mfpu=neon-vfpv4 $< -c -o $@",
            "sdboot_t117.bin",
        ]
    )
    elf = inputs_build.require_file(reader / "obj/sdboot_t117.elf")
    symbols = subprocess.run(
        ["arm-none-eabi-nm", "-n", str(elf)],
        capture_output=True,
        check=True,
        text=True,
        timeout=20,
    ).stdout
    if not any(line.split() == ["00006200", "T", "_start"] for line in symbols.splitlines()):
        fail("microSD reader does not start at its expected entry address")
    if "sdio_write_block" in symbols:
        fail("microSD reader unexpectedly contains the card-write function")
    return inputs_build.require_file(reader / "sdboot_t117.bin")


def build_bootstrap(  # noqa: PLR0913 -- source selection and payload inputs stay explicit.
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    *,
    work: Path,
    zimage: Path,
    dtb: Path,
    uboot: UbootBuild | None = None,
    kernel_output: Path | None = None,
    rootfs_output: Path | None = None,
    early_firmware: Path | None = None,
) -> BootstrapBuild:
    """Build and verify the declarative bootstrap contract."""
    platform_bootstrap = platform["bootstrap"]
    target_bootstrap = target_config["bootstrap"]
    bootstrap_work = work / "bootstrap"
    if bootstrap_work.is_symlink():
        fail(f"generated bootstrap path must not be a symlink: {bootstrap_work}")
    if bootstrap_work.exists():
        if not bootstrap_work.is_dir():
            fail(f"generated bootstrap path is not a directory: {bootstrap_work}")
        shutil.rmtree(bootstrap_work)

    if platform_bootstrap.get("backend") == "sc9820e":
        from .sc9820e import build_sc9820e  # noqa: PLC0415 -- selected backend only.

        if kernel_output is None or rootfs_output is None:
            fail("SC9820E bootstrap requires kernel and rootfs output directories")
        return build_sc9820e(
            target_config,
            work=bootstrap_work,
            kernel=zimage,
            dtb=dtb,
            kernel_output=kernel_output,
            rootfs_output=rootfs_output,
            early_firmware=early_firmware,
        )

    bootstrap = bootstrap_work / platform_bootstrap["source_destination"]
    vendor = bootstrap_work / platform_bootstrap["vendor_destination"]
    projected_output = bootstrap_work / platform_bootstrap["output_destination"]
    shutil.copytree(
        inputs_build.require_directory(inputs_build.root_source(target_bootstrap["source"])),
        bootstrap,
    )
    for step in platform_bootstrap["shared_copies"]:
        source = inputs_build.root_source(step["source"])
        destination = bootstrap / step["destination"]
        if source.is_dir() and not source.is_symlink():
            shutil.copytree(source, destination)
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(inputs_build.require_file(source), destination)
    sources_build.write_generated_files(
        bootstrap,
        generated_bootstrap_files(target_config, platform),
        owner="bootstrap generated inputs",
    )
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("U-Boot stage0 requires a verified full U-Boot artifact")
        sources_build.write_generated_files(
            bootstrap,
            {"generated/fplinux-uboot-build.h": uboot_build_header(uboot)},
            owner="U-Boot stage0",
        )

    vendor_lock = sources_build.source_lock_entry(
        sources, platform_bootstrap["vendor_source_lock"]
    )
    archive = acquire_source(
        common.ROOT, platform_bootstrap["vendor_source_lock"], inputs_build.CACHE
    )
    commit = vendor_lock.get("commit")
    if not isinstance(commit, str) or not commit:
        fail("bootstrap vendor commit must be a non-empty string")
    prefix = platform_bootstrap["archive_prefix"].replace("{commit}", commit)
    files = list(platform_bootstrap["files"])
    lcd_config = target_bootstrap.get("lcd_config")
    lcd_destination = platform_bootstrap["lcd_config_destination"]
    if lcd_config is None:
        files.append(lcd_destination)
    sources_build.extract_vendor(archive, prefix, files, vendor)
    if lcd_config is not None:
        destination = vendor / lcd_destination
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(
            inputs_build.require_file(inputs_build.target_source(target, lcd_config)), destination
        )
        panel_data = render_bootstrap_display(target_config["display"])
        if panel_data is not None:
            (destination.parent / "fplinux-panel-data.h").write_bytes(panel_data)
    sources_build.apply_patches(
        vendor,
        [
            inputs_build.require_file(inputs_build.root_source(relative))
            for relative in platform_bootstrap["patches"]
        ],
    )

    projected_output.mkdir(parents=True, exist_ok=True)
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("U-Boot stage0 lost its verified full U-Boot artifact")
        shutil.copyfile(uboot.binary, projected_output / "u-boot.bin")
    else:
        if rootfs_output is None:
            fail("UMS9117 RAM boot requires the root filesystem output")
        initramfs = inputs_build.require_file(rootfs_output / INITRAMFS_NAME)
        layout = effective_boot_layout(target_config)
        if zimage.stat().st_size > layout["kernel_size"] - ZIMAGE_WORKSPACE_BYTES:
            fail("UMS9117 zImage and decompression workspace do not fit in RAM")
        initramfs_start = layout["kernel_load"] + layout["kernel_size"]
        capsule_bytes = inputs_build.require_file(work / "device-data.cpio").stat().st_size
        required_bytes = initramfs.stat().st_size + ((capsule_bytes + 3) & ~3)
        if required_bytes > layout["fdt_load"] - initramfs_start:
            fail("UMS9117 initramfs and device data do not fit between zImage and DTB")
        shutil.copyfile(zimage, projected_output / target_bootstrap["kernel_destination"])
        shutil.copyfile(dtb, projected_output / target_bootstrap["dtb_destination"])
        shutil.copyfile(initramfs, projected_output / INITRAMFS_NAME)
    display = bootstrap_display(target_config)
    if display["width"]:
        screen = f"{display['width']}x{display['height']}"
        profile = bootstrap / "fplinux-status/fplinux-ui-profile.json"
        process_build.run(
            [
                "python3",
                str(bootstrap / "fplinux_status_assets.py"),
                "--tokens",
                str(profile),
                "--sources",
                "/opt/quality/src/ui-fonts",
                "--output",
                str(bootstrap / "generated"),
                "--profile",
                screen,
                "--lv-font-conv",
                "/opt/quality/node-tools/node_modules/.bin/lv_font_conv",
            ]
        )
        process_build.run(
            [
                "python3",
                str(bootstrap / "fplinux-status/generate-brand.py"),
                "--profile",
                str(profile),
                "--masters",
                str(bootstrap / "fplinux-status/brand"),
                "--boot-out",
                str(bootstrap / "generated"),
                "--screen",
                screen,
            ],
            environment={
                "NODE_PATH": "/opt/quality/node-tools/node_modules",
                "PYTHONPATH": str(bootstrap / "fplinux-status/brand"),
            },
        )
    process_build.run(
        [
            "make",
            "-C",
            str(vendor / platform_bootstrap["pack_reloc"]),
            "clean",
            "all",
        ]
    )
    _compile_stage(bootstrap, platform_bootstrap, target_bootstrap, entry="usb")
    ramboot = inputs_build.require_file(bootstrap / target_bootstrap["image"])
    ramboot_map = inputs_build.require_file(bootstrap / target_bootstrap["map"])
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("U-Boot stage0 lost its verified full U-Boot artifact")
        personalization = verify_sd_stage0_image(
            ramboot,
            uboot,
            map_file=ramboot_map,
            load_address=target_bootstrap["load_address"],
            payload_limit=target_bootstrap["payload_limit"],
            layout=target_config["layout"],
        )
    else:
        personalization = verify_images(
            ramboot,
            zimage=zimage,
            dtb=dtb,
            initramfs=initramfs,
            map_file=ramboot_map,
            load_address=target_bootstrap["load_address"],
            payload_limit=target_bootstrap["payload_limit"],
            forbidden_markers=target_config["linux"]["forbidden_dtb_markers"],
        )
    cold_sd = None
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("Autonomous stage0 requires the same verified U-Boot artifact")
        usb_image = projected_output / "usb-stage0.bin"
        usb_map = projected_output / "usb-stage0.map"
        shutil.copyfile(ramboot, usb_image)
        shutil.copyfile(ramboot_map, usb_map)
        _compile_stage(bootstrap, platform_bootstrap, target_bootstrap, entry="sd")
        verify_sd_stage0_image(
            ramboot,
            uboot,
            map_file=ramboot_map,
            load_address=target_bootstrap["load_address"],
            payload_limit=target_bootstrap["payload_limit"],
            layout=target_config["layout"],
        )
        reader = _build_card_reader(archive, prefix, files, bootstrap_work / "card-reader")
        cold_sd = ColdSdBootstrap(reader, ramboot)
        ramboot, ramboot_map = usb_image, usb_map
    return BootstrapBuild(ramboot, ramboot_map, personalization, cold_sd=cold_sd)

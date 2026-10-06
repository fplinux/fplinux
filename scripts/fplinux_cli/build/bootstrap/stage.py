# SPDX-License-Identifier: GPL-2.0-only
"""Prepare, compile and verify the selected pre-Linux boot payload."""

from __future__ import annotations

import shutil
import tarfile
from typing import TYPE_CHECKING, Any

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.common import fail

from .recipe import generated_bootstrap_files, uboot_build_header
from .verify import verify_images, verify_sd_stage0_image

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.storage.uboot import UbootBuild


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
) -> tuple[Path, Path, dict[str, int | str]]:
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

    bootstrap = bootstrap_work / platform_bootstrap["source_destination"]
    vendor = bootstrap_work / platform_bootstrap["vendor_destination"]
    projected_output = bootstrap_work / platform_bootstrap["output_destination"]
    shutil.copytree(
        inputs_build.require_directory(
            inputs_build.target_source(target, target_bootstrap["source"])
        ),
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
    linux_lock = sources_build.source_lock_entry(sources, platform["linux"]["source_lock"])
    linux_archive = sources_build.fetch(
        linux_lock.get("url"),
        linux_lock.get("sha256"),
        inputs_build.CACHE / "downloads/linux",
        f"linux-{linux_lock['version']}.tar.xz",
    )
    with tarfile.open(linux_archive, "r:*") as source_archive:
        for step in platform_bootstrap["linux_copies"]:
            member = source_archive.getmember(f"linux-{linux_lock['version']}/{step['source']}")
            stream = source_archive.extractfile(member)
            if stream is None or not member.isfile():
                fail(f"invalid bootstrap Linux font member: {step['source']}")
            destination = bootstrap / step["destination"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(stream.read())
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
    archive = sources_build.fetch(
        vendor_lock.get("archive_url"),
        vendor_lock.get("archive_sha256"),
        inputs_build.CACHE / "downloads",
        platform_bootstrap["vendor_cache_name"],
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
        shutil.copyfile(zimage, projected_output / target_bootstrap["kernel_destination"])
        shutil.copyfile(dtb, projected_output / target_bootstrap["dtb_destination"])
    process_build.run(
        [
            "make",
            "-C",
            str(vendor / platform_bootstrap["pack_reloc"]),
            "clean",
            "all",
        ]
    )
    process_build.run(["make", "-C", str(bootstrap), platform_bootstrap["safety_target"]])
    process_build.run(
        [
            "make",
            "-C",
            str(bootstrap),
            *platform_bootstrap["build_targets"],
            f"TOOLCHAIN={target_bootstrap['toolchain']}",
            f"LTO={target_bootstrap['lto']}",
        ]
    )
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
            map_file=ramboot_map,
            load_address=target_bootstrap["load_address"],
            payload_limit=target_bootstrap["payload_limit"],
            forbidden_markers=target_config["linux"]["forbidden_dtb_markers"],
        )
    return ramboot, ramboot_map, personalization

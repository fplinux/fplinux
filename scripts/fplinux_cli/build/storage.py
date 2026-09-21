# SPDX-License-Identifier: GPL-2.0-only
"""Build microSD boot and root-filesystem artifacts."""

from __future__ import annotations

import os
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import alpine_state, common, profile_layout
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.inputs import fail
from fplinux_cli.device_tree import DeviceTreeError

if TYPE_CHECKING:
    from fplinux_cli.uboot_tools import UbootBuild


def build_profile_uboot(
    target: str, target_config: dict[str, Any], work: Path, jobs: int
) -> UbootBuild | None:
    """Build the full U-Boot selected by one profile."""
    config = target_config["uboot"]
    if config["kind"] == "none":
        return None
    if config["kind"] != "full":
        fail(f"unsupported U-Boot profile kind: {config['kind']}")
    lock = config["lock"]
    archive = sources_build.fetch(
        lock["archive_url"],
        lock["archive_sha256"],
        inputs_build.CACHE / "downloads/uboot",
        "source.tar.bz2",
    )
    container_recipe = inputs_build.require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE"),
        "container image recipe",
    )
    from fplinux_cli import uboot_tools  # noqa: PLC0415 -- profile-only source.

    try:
        profile = inputs_build.selected_profile(target_config)
        if profile is None:
            fail("full U-Boot requires a selected profile")
        target_root = common.ROOT / "targets" / target
        projections = [
            (inputs_build.require_file(common.ROOT / step["source"]), step["destination"])
            for step in config["copies"]
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work, prefix=".uboot-inputs.") as name:
            generated = Path(name)
            defconfig = generated / Path(config["defconfig"]).name
            layout_header = generated / "fplinux-boot-layout.h"
            layout_dtsi = generated / "fplinux-uboot-layout.dtsi"
            target_header = generated / "fplinux-uboot-target.h"
            defconfig.write_bytes(
                profile_layout.uboot_defconfig(
                    inputs_build.require_file(target_root / config["defconfig"]).read_bytes(),
                    target_config["layout"],
                )
            )
            layout_header.write_bytes(profile_layout.boot_layout_header(target_config["layout"]))
            layout_dtsi.write_bytes(profile_layout.uboot_layout_dtsi(target_config["layout"]))
            target_header.write_text(
                f'#define FPLINUX_UBOOT_TARGET "{target}"\n', encoding="ascii"
            )
            projections.append((layout_header, "include/fplinux-boot-layout.h"))
            projections.append((layout_dtsi, "arch/arm/dts/fplinux-uboot-layout.dtsi"))
            projections.append((target_header, "include/fplinux-uboot-target.h"))
            uboot = uboot_tools.build_full(
                archive,
                config,
                defconfig=defconfig,
                projections=projections,
                patches=[
                    inputs_build.require_file(common.ROOT / path) for path in config["patches"]
                ],
                work=work,
                jobs=jobs,
                container_recipe=container_recipe,
                cross_compile="arm-none-eabi-",
                layout=target_config["layout"],
            )
    except (
        OSError,
        subprocess.SubprocessError,
        tarfile.TarError,
        uboot_tools.UbootToolsError,
    ) as error:
        fail(str(error))
    process_build.log_message(f"U-Boot build ready: {uboot.receipt['recipe'][:16]}")
    return uboot


def profile_ext4_artifact(
    target_config: dict[str, Any],
    work: Path,
    rootfs_output: Path,
    rootfs_recipe: str,
) -> Path | None:
    """Recheck and return the selected ext4 artifact."""
    config = target_config["image"]
    if config["kind"] == "none":
        return None
    if config["kind"] != "ext4-root":
        fail(f"unsupported profile image kind: {config['kind']}")
    from fplinux_cli import ext4_root  # noqa: PLC0415 -- profile-only source.

    try:
        rootfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
        plan = ext4_root.create_plan(
            config,
            rootfs_recipe,
            rootfs_receipt,
            inputs_build.require_sha256(
                os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE"),
                "container image recipe",
            ),
        )
        output = work / "rootfs-image"
        ext4_root.receipt_identity(output, plan)
    except (OSError, ext4_root.Ext4RootError) as error:
        fail(str(error))
    return inputs_build.require_file(output / config["filename"])


def build_profile_fit(  # noqa: PLR0913 -- target selection and artifact paths stay explicit.
    target: str,
    target_config: dict[str, Any],
    *,
    work: Path,
    zimage: Path,
    dtb: Path,
    uboot: UbootBuild | None,
) -> Path | None:
    """Build and recheck the native FIT selected by one profile."""
    config = target_config["fit"]
    if config["kind"] == "none":
        return None
    if config["kind"] != "sha256" or uboot is None:
        fail("SHA-256 FIT requires verified U-Boot tools")
    from fplinux_cli import fit_image  # noqa: PLC0415 -- profile-only source.

    try:
        plan = fit_image.create_plan(
            target,
            target_config["identity"]["display_name"],
            config,
            zimage=zimage,
            dtb=dtb,
            tools_receipt=uboot.receipt,
        )
        output = work / "fit"
        fit = fit_image.build(
            mkimage=uboot.mkimage,
            dumpimage=uboot.dumpimage,
            zimage=zimage,
            dtb=dtb,
            output=output,
            plan=plan,
        )
        fit_image.receipt_identity(output, plan)
    except (
        OSError,
        subprocess.SubprocessError,
        DeviceTreeError,
        fit_image.FitImageError,
    ) as error:
        fail(str(error))
    return inputs_build.require_file(fit)


def build_profile_sd_image(
    target_config: dict[str, Any],
    work: Path,
    fit: Path | None,
    ext4: Path | None,
) -> Path | None:
    """Assemble the selected whole-card image from verified profile artifacts."""
    image_config = target_config["image"]
    if image_config["kind"] == "none":
        return None
    if image_config["kind"] != "ext4-root" or fit is None or ext4 is None:
        fail("whole-card image requires FIT and ext4 root artifacts")
    storage = target_config["storage"]
    if not isinstance(storage, dict):
        fail("whole-card image requires a storage layout")
    from fplinux_cli import sd_image  # noqa: PLC0415 -- profile-only source.

    destination = work / "sd-image" / sd_image.compressed_image_name(storage)
    try:
        return inputs_build.require_file(
            sd_image.build(
                fit,
                ext4,
                destination,
                fit_spec=target_config["fit"],
                storage=storage,
            )
        )
    except (OSError, subprocess.SubprocessError, sd_image.SdImageError) as error:
        fail(str(error))


def profile_boot_artifact_set(
    target_config: dict[str, Any],
    disk_image: Path | None,
) -> tuple[dict[str, Path], dict[str, Any]]:
    """Collect the profile payload consumed by package and run."""
    files: dict[str, Path] = {}
    image_config = target_config["image"]
    if image_config["kind"] == "ext4-root":
        if disk_image is None:
            fail("whole-card image artifact is missing")
        image_bundle_path = disk_image.name
        files[image_bundle_path] = disk_image
        required = [image_bundle_path]
    else:
        required = []

    metadata = {
        "required": required,
        "runnable": target_config["runtime"]["runnable"],
    }
    return files, metadata


def default_boot_artifacts() -> dict[str, Any]:
    """Return the ordinary RAM pipeline contract for callers without extra artifacts."""
    return {
        "required": [],
        "runnable": True,
    }

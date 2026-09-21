# SPDX-License-Identifier: GPL-2.0-only
"""Build and validate the pre-Linux boot payload."""

from __future__ import annotations

import shutil
import struct
from typing import TYPE_CHECKING, Any

from fplinux_cli import profile_layout
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.inputs import fail
from fplinux_cli.common import canonical_json_bytes, sha256_bytes, sha256_file
from fplinux_cli.device_tree import DeviceTreeError, exact_path_properties
from fplinux_cli.identity import IdentityError
from fplinux_cli.identity_codegen import BOOTSTRAP_IDENTITY_HEADER, bootstrap_identity_header

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.uboot_tools import UbootBuild


RAM_SESSION_BYTES = 512
RAM_SESSION_ALIGNMENT = 64
RAM_SESSION_RNG_SEED_MARKER = bytes([0xA1]) * 64
RAM_SESSION_DTB_MARKERS = {
    "fplinux,ssh-client-key": bytes([0xB2]) * 68,
    "fplinux,session-id": bytes([0xC3]) * 32,
    "fplinux,usb-session": bytes([0xD4]) * 256,
}


def bootstrap_tree_entries(source: Path) -> list[dict[str, int | str]]:
    """Describe the bytes and modes copied from one bootstrap source tree."""
    source = inputs_build.require_directory(source)
    entries: list[dict[str, int | str]] = [
        {"path": ".", "type": "directory", "mode": source.stat().st_mode & 0o777}
    ]
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source).as_posix()
        if path.is_dir():
            entries.append(
                {
                    "path": relative,
                    "type": "directory",
                    "mode": path.stat().st_mode & 0o777,
                }
            )
        elif path.is_file():
            entries.append(
                {
                    "path": relative,
                    "type": "file",
                    "mode": path.stat().st_mode & 0o777,
                    "sha256": sha256_file(path),
                }
            )
        else:
            fail(f"bootstrap source entry is not a regular file or directory: {path}")
    return entries


def generated_bootstrap_identity(target_config: dict[str, Any]) -> bytes:
    """Return the exact target identity header consumed by bootstrap C."""
    try:
        return bootstrap_identity_header(
            target_config["identity"], target_config["bootstrap"]["record_prefix"]
        )
    except IdentityError as error:
        fail(str(error))
    return b""


def effective_boot_layout(
    target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, int]:
    """Return the one layout selected for this bootstrap build."""
    layout = target_config.get("layout")
    return layout if isinstance(layout, dict) else platform["bootstrap"]["layout"]


def generated_bootstrap_files(
    target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, bytes]:
    """Return every transient input generated for one bootstrap build."""
    layout = effective_boot_layout(target_config, platform)
    return {
        BOOTSTRAP_IDENTITY_HEADER: generated_bootstrap_identity(target_config),
        "generated/fplinux-boot-layout.h": profile_layout.boot_layout_header(layout),
        "generated/fplinux-bootstrap-memory.ld": profile_layout.bootstrap_memory_ld(
            target_config["bootstrap"], layout
        ),
    }


def bootstrap_recipe_digest(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> str:
    """Hash exactly the bootstrap inputs which can change the RAM image."""
    platform_bootstrap = platform["bootstrap"]
    target_bootstrap = target_config["bootstrap"]
    vendor_source = sources_build.source_lock_entry(
        sources, platform_bootstrap["vendor_source_lock"]
    )
    vendor_commit = vendor_source.get("commit")
    if not isinstance(vendor_commit, str) or not vendor_commit:
        fail("bootstrap vendor commit must be a non-empty string")
    shared_copies: list[dict[str, object]] = []
    for step in platform_bootstrap["shared_copies"]:
        source = inputs_build.root_source(step["source"])
        copied: dict[str, object] = {
            "source": step["source"],
            "destination": step["destination"],
        }
        if source.is_dir() and not source.is_symlink():
            copied["tree"] = bootstrap_tree_entries(source)
        else:
            copied["sha256"] = sha256_file(inputs_build.require_file(source))
        shared_copies.append(copied)
    generated = {
        path: sha256_bytes(contents)
        for path, contents in generated_bootstrap_files(target_config, platform).items()
    }
    manifest = {
        "target": target,
        "target_bootstrap": target_bootstrap,
        "platform_bootstrap": platform_bootstrap,
        "target_source": bootstrap_tree_entries(
            inputs_build.target_source(target, target_bootstrap["source"])
        ),
        "shared_copies": shared_copies,
        "patches": [
            {
                "path": relative,
                "sha256": sha256_file(
                    inputs_build.require_file(inputs_build.root_source(relative))
                ),
            }
            for relative in platform_bootstrap["patches"]
        ],
        "vendor_source": {
            "commit": vendor_commit,
            "archive_sha256": inputs_build.require_sha256(
                vendor_source.get("archive_sha256"),
                "bootstrap vendor source",
            ),
        },
        "generated": generated,
        "implementation": {
            "scripts/fplinux_cli/build_env.py": sha256_file(
                inputs_build.root_source("scripts/fplinux_cli/build_env.py")
            ),
            **{name: sha256_file(path) for name, path in inputs_build.implementation_sources()},
        },
    }
    return sha256_bytes(canonical_json_bytes(manifest))


def _verify_session_dtb(tree: bytes) -> None:
    """Require one canonical marker for each RAM-session DT property."""
    try:
        properties = exact_path_properties(tree, ("/chosen", "/fplinux-session"))
    except DeviceTreeError as error:
        fail(str(error))
    chosen = properties["/chosen"]
    session = properties["/fplinux-session"]
    if chosen.get("rng-seed") != RAM_SESSION_RNG_SEED_MARKER:
        fail("target DTB /chosen rng-seed does not contain its canonical marker")
    if tree.count(RAM_SESSION_RNG_SEED_MARKER) != 1:
        fail("target DTB marker rng-seed must occur exactly once")
    if session.get("compatible") != b"fplinux,ram-session\0":
        fail("target DTB /fplinux-session has an invalid compatible")
    for name, marker in RAM_SESSION_DTB_MARKERS.items():
        if session.get(name) != marker:
            fail(f"target DTB /fplinux-session {name} does not contain its canonical marker")
        if tree.count(marker) != 1:
            fail(f"target DTB marker {name} must occur exactly once")


def verify_images(  # noqa: PLR0913 -- artifacts and target limits stay explicit.
    ramboot: Path,
    *,
    zimage: Path,
    dtb: Path,
    map_file: Path,
    load_address: int,
    payload_limit: int,
    forbidden_markers: list[str],
) -> dict[str, int | str]:
    """Enforce the generic RAM-only bootstrap image contract."""
    image = ramboot.read_bytes()
    kernel = zimage.read_bytes()
    tree = dtb.read_bytes()
    if image[:4] != b"DHTB" or len(image) < 0x200:
        fail("bootstrap output does not have a complete DHTB header")
    declared = struct.unpack_from("<I", image, 0x30)[0]
    if declared + 0x200 > len(image):
        fail("DHTB declared size exceeds the RAM image")
    if load_address + len(image) >= payload_limit:
        fail("RAM image reaches its declared payload limit")
    if len(kernel) < 0x28 or kernel[0x24:0x28] != b"\x18\x28\x6f\x01":
        fail("zImage has invalid ARM magic")
    if tree[:4] != b"\xd0\x0d\xfe\xed":
        fail("target DTB has invalid FDT magic")
    lowered = tree.lower()
    for marker in forbidden_markers:
        if marker.lower().encode() in lowered:
            fail(f"target DTB contains forbidden storage marker {marker}")

    symbols = _bootstrap_map_symbols(map_file)
    required = {
        "__image_start",
        "linux_zimage_start",
        "linux_zimage_end",
        "linux_dtb_start",
        "linux_dtb_end",
        "fplinux_session_start",
        "fplinux_session_end",
        "FPLINUX_BOOTSTRAP_STORAGE_DISABLED",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        fail(f"bootstrap map lacks symbols: {', '.join(missing)}")
    if symbols["FPLINUX_BOOTSTRAP_STORAGE_DISABLED"] != 1:
        fail("bootstrap storage-disabled link marker is not one")
    if symbols["__image_start"] != load_address:
        fail("bootstrap map image start differs from target load_address")

    zimage_start = symbols["linux_zimage_start"]
    zimage_end = symbols["linux_zimage_end"]
    dtb_start = symbols["linux_dtb_start"]
    dtb_end = symbols["linux_dtb_end"]
    for name, start, end in (
        ("zImage", zimage_start, zimage_end),
        ("DTB", dtb_start, dtb_end),
    ):
        if not load_address <= start <= end <= load_address + len(image):
            fail(f"embedded {name} range lies outside the RAM image")
    if zimage_end - zimage_start != len(kernel):
        fail("embedded zImage size differs from the built zImage")
    if dtb_end - dtb_start != len(tree):
        fail("embedded DTB size differs from the built DTB")
    if image[zimage_start - load_address : zimage_end - load_address] != kernel:
        fail("embedded zImage bytes differ from the built zImage")
    if image[dtb_start - load_address : dtb_end - load_address] != tree:
        fail("embedded DTB bytes differ from the built DTB")

    session_start = symbols["fplinux_session_start"]
    session_end = symbols["fplinux_session_end"]
    expected_start = (dtb_end + RAM_SESSION_ALIGNMENT - 1) & -RAM_SESSION_ALIGNMENT
    if session_start != expected_start or session_start % RAM_SESSION_ALIGNMENT:
        fail("RAM-session slot does not immediately follow the aligned embedded DTB")
    if session_end - session_start != RAM_SESSION_BYTES:
        fail("RAM-session slot does not have its exact ABI size")
    if not load_address <= session_start < session_end <= load_address + len(image):
        fail("RAM-session slot lies outside the RAM image")
    offset = session_start - load_address
    template = image[offset : offset + RAM_SESSION_BYTES]
    if template != bytes(RAM_SESSION_BYTES):
        fail("canonical RAM-session slot is not all zero")
    _verify_session_dtb(tree)
    return {
        "offset": offset,
        "bytes": RAM_SESSION_BYTES,
        "template_sha256": sha256_bytes(template),
    }


def uboot_build_header(uboot: UbootBuild) -> bytes:
    """Generate exact stage0 constants from the verified U-Boot artifact."""
    license_tag = "SPDX-License-" + "Identifier"
    return (
        f"/* {license_tag}: GPL-2.0-only */\n"
        "/* Generated from the selected full U-Boot artifact. */\n"
        "#ifndef FPLINUX_UBOOT_BUILD_H\n"
        "#define FPLINUX_UBOOT_BUILD_H\n\n"
        f"#define FPLINUX_UBOOT_ENTRY_PHYS 0x{uboot.entry:08x}U\n"
        f"#define FPLINUX_UBOOT_BINARY_BYTES {uboot.binary.stat().st_size}U\n"
        "\n"
        "#endif\n"
    ).encode("ascii")


def _bootstrap_map_symbols(map_file: Path) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for line in map_file.read_text().splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            symbols[fields[2]] = int(fields[0], 16)
        except ValueError:
            continue
    return symbols


def verify_sd_stage0_image(  # noqa: PLR0913 -- artifacts and target limits stay explicit.
    ramboot: Path,
    uboot: UbootBuild,
    *,
    map_file: Path,
    load_address: int,
    payload_limit: int,
    layout: dict[str, int],
) -> dict[str, int | str]:
    """Verify the resident stage0, embedded U-Boot and session slot."""
    image = ramboot.read_bytes()
    binary = uboot.binary.read_bytes()
    if image[:4] != b"DHTB" or len(image) < 0x200:
        fail("U-Boot stage0 output does not have a complete DHTB header")
    declared = struct.unpack_from("<I", image, 0x30)[0]
    if declared + 0x200 > len(image):
        fail("U-Boot stage0 DHTB declared size exceeds the RAM image")
    if load_address + len(image) >= payload_limit:
        fail("U-Boot stage0 reaches its declared payload limit")

    symbols = _bootstrap_map_symbols(map_file)
    required = {
        "__image_start",
        "__bss_end",
        "uboot_payload_start",
        "uboot_payload_end",
        "fplinux_session_start",
        "fplinux_session_end",
        "stage0_ops",
        "uboot_handoff",
        "FPLINUX_BOOTSTRAP_STORAGE_DISABLED",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        fail(f"U-Boot stage0 map lacks symbols: {', '.join(missing)}")
    if symbols["FPLINUX_BOOTSTRAP_STORAGE_DISABLED"] != 1:
        fail("U-Boot stage0 storage-disabled marker is not one")
    if symbols["__image_start"] != load_address:
        fail("U-Boot stage0 map image start differs from target load_address")
    if symbols["__bss_end"] > layout["uboot_stack"]:
        fail("resident U-Boot stage0 overlaps the full U-Boot stack")

    uboot_start = symbols["uboot_payload_start"]
    uboot_end = symbols["uboot_payload_end"]
    if not load_address <= uboot_start < uboot_end <= load_address + len(image):
        fail("embedded full U-Boot range lies outside stage0")
    if uboot_end - uboot_start != len(binary):
        fail("embedded full U-Boot size differs from its verified binary")
    if image[uboot_start - load_address : uboot_end - load_address] != binary:
        fail("embedded full U-Boot bytes differ from its verified binary")

    session_start = symbols["fplinux_session_start"]
    session_end = symbols["fplinux_session_end"]
    expected_start = (uboot_end + RAM_SESSION_ALIGNMENT - 1) & -RAM_SESSION_ALIGNMENT
    if session_start != expected_start or session_start % RAM_SESSION_ALIGNMENT:
        fail("stage0 session slot does not immediately follow embedded U-Boot")
    if session_end - session_start != RAM_SESSION_BYTES:
        fail("stage0 session slot does not have its exact ABI size")
    offset = session_start - load_address
    template = image[offset : offset + RAM_SESSION_BYTES]
    if template != bytes(RAM_SESSION_BYTES):
        fail("canonical stage0 session slot is not all zero")
    for name in ("stage0_ops", "uboot_handoff"):
        if not load_address <= symbols[name] < symbols["__bss_end"]:
            fail(f"resident {name} lies outside the stage0 image")
    return {
        "offset": offset,
        "bytes": RAM_SESSION_BYTES,
        "template_sha256": sha256_bytes(template),
    }


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
    sources_build.extract_vendor(archive, prefix, platform_bootstrap["files"], vendor)
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

# SPDX-License-Identifier: GPL-2.0-only
"""Describe the causal sources and generated inputs of the bootstrap payload."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.identity import BOOTSTRAP_IDENTITY_HEADER, bootstrap_identity_header
from fplinux_cli.build.storage import layout as profile_layout
from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes, sha256_file
from fplinux_cli.manifests.identity import IdentityError

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.storage.uboot import UbootBuild


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
    linux_source = sources_build.source_lock_entry(sources, platform["linux"]["source_lock"])
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
    lcd_config = target_bootstrap.get("lcd_config")
    manifest = {
        "target": target,
        "target_bootstrap": target_bootstrap,
        "platform_bootstrap": platform_bootstrap,
        "target_source": bootstrap_tree_entries(
            inputs_build.target_source(target, target_bootstrap["source"])
        ),
        "shared_copies": shared_copies,
        "lcd_config": (
            {
                "source": lcd_config,
                "sha256": sha256_file(
                    inputs_build.require_file(inputs_build.target_source(target, lcd_config))
                ),
            }
            if lcd_config is not None
            else None
        ),
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
        "linux_source": {
            "version": linux_source["version"],
            "sha256": inputs_build.require_sha256(
                linux_source.get("sha256"), "bootstrap Linux font source"
            ),
        },
        "generated": generated,
        "implementation": {
            name: sha256_file(path)
            for name, path in inputs_build.bootstrap_implementation_sources()
        },
    }
    return sha256_bytes(canonical_json_bytes(manifest))


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

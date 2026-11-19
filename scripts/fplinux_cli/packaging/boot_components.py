# SPDX-License-Identifier: GPL-2.0-only
"""Record reusable ranges in the verified phone image without altering it."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.newc import newc_entries
from fplinux_cli.common import canonical_json_bytes, fail, relative_name, sha256_bytes

if TYPE_CHECKING:
    from pathlib import Path

BOOT_COMPONENTS = "boot-components.json"


def _built_component(bundle: Path, manifest: dict[str, Any], key: str) -> bytes:
    name = relative_name(manifest["kernel_artifacts"][key], field=f"built {key}")
    path = bundle / name
    if path.is_symlink() or not path.is_file():
        fail(f"missing built image component: {name}")
    data = path.read_bytes()
    record = manifest["files"][name]
    if record["sha256"] != sha256_bytes(data) or record["size"] != len(data):
        fail(f"built image component differs from the build manifest: {name}")
    return data


def exact_component_offset(image: bytes, component: bytes, label: str) -> int:
    """Locate one occurrence of the verified component in the built payload."""
    offset = image.find(component)
    if not component or offset < 0 or image.find(component, offset + 1) >= 0:
        fail(f"RAM image must contain exactly one verified {label}")
    return offset


def boot_components(
    bundle: Path,
    manifest: dict[str, Any],
    *,
    platform: str,
    image_path: str,
    image: bytes,
) -> bytes:
    """Locate the exact built kernel and common SquashFS inside a RAM image."""
    kernel = _built_component(bundle, manifest, "image")
    initramfs = _built_component(bundle, manifest, "initramfs")
    try:
        roots = [entry for entry in newc_entries(initramfs) if entry.name == "root.squashfs"]
    except ValueError as error:
        fail(f"cannot read the built RAM root: {error}")
    if len(roots) != 1 or not roots[0].data.startswith(b"hsqs"):
        fail("built RAM root must contain one SquashFS base")
    root = roots[0]
    return canonical_json_bytes(
        {
            "platform": platform,
            "image": image_path,
            "kernel": [exact_component_offset(image, kernel, "kernel"), len(kernel)],
            "root": [
                exact_component_offset(image, initramfs, "initramfs") + root.offset,
                len(root.data),
            ],
        }
    )

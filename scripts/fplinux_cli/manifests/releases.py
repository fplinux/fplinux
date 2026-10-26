# SPDX-License-Identifier: GPL-2.0-only
"""Load target-owned release contents."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Sequence

from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.paths import target_release_manifest_path
from fplinux_cli.manifests.values import exact_table, path_array, relative_value


def load_release(target: str, *, bundle_packages: Sequence[str] = ()) -> dict[str, Any]:
    """Load target release contents and the resolved optional APK set."""
    path = target_release_manifest_path(target)
    if path.is_symlink() or not path.is_file():
        fail(f"target release manifest is missing or invalid: {path}")
    raw = load_toml(path)
    manifest = exact_table(
        raw,
        {"image", "bundle_files", "runtime_files", "documents"},
        f"target {target} release manifest",
    )
    image = relative_value(manifest.get("image"), "release manifest image")
    bundle_files = path_array(manifest.get("bundle_files"), "release bundle_files")
    runtime_files = path_array(manifest.get("runtime_files"), "release runtime_files")
    documents = path_array(manifest.get("documents"), "release documents", allow_empty=True)
    if image not in runtime_files:
        fail("release manifest image must be a runtime file")
    if not set(runtime_files).issubset(bundle_files):
        fail("release runtime files must be bundle files")
    if any(relative.startswith("apks/") for relative in bundle_files):
        fail("release APK files must come from the resolved bundle package set")
    apk_files = path_array(
        [f"apks/{package}.apk" for package in bundle_packages],
        "release bundle APK files",
        allow_empty=True,
    )
    bundle_files = [*bundle_files, *apk_files]
    return {
        "image": image,
        "bundle_files": bundle_files,
        "runtime_files": runtime_files,
        "documents": documents,
    }

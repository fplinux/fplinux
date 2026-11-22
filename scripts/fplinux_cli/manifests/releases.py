# SPDX-License-Identifier: GPL-2.0-only
"""Load target-owned release contents."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Sequence

from fplinux_cli.artifacts.card_kit import CARD_APK_INDEX, CARD_APK_KEY
from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.paths import target_release_manifest_path
from fplinux_cli.manifests.values import exact_table, path_array, relative_value


def load_release(target: str, *, bundle_packages: Sequence[str] = ()) -> dict[str, Any]:
    """Load target release contents and the resolved optional APK set."""
    path = target_release_manifest_path(target)
    if path.is_symlink() or not path.is_file():
        fail(f"target release manifest is missing or invalid: {path}")
    manifest = exact_table(
        {"publish": True, **load_toml(path)},
        {"publish", "image", "bundle_files", "documents"},
        f"target {target} release manifest",
    )
    publish = manifest["publish"]
    if not isinstance(publish, bool):
        fail("release manifest publish must be a boolean")
    image = relative_value(manifest.get("image"), "release manifest image")
    bundle_files = path_array(manifest.get("bundle_files"), "release bundle_files")
    documents = path_array(manifest.get("documents"), "release documents", allow_empty=True)
    if image not in bundle_files:
        fail("release manifest image must be a bundle file")
    if any(relative.startswith("apks/") for relative in bundle_files):
        fail("release APK files must come from the resolved bundle package set")
    apk_files = path_array(
        [f"apks/{package}.apk" for package in bundle_packages],
        "release bundle APK files",
        allow_empty=True,
    )
    if apk_files:
        bundle_files = [*bundle_files, *apk_files, CARD_APK_INDEX, CARD_APK_KEY]
    return {
        "publish": publish,
        "image": image,
        "bundle_files": bundle_files,
        "documents": documents,
    }

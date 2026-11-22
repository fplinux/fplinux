# SPDX-License-Identifier: GPL-2.0-only
"""Load target release policy and resolve the standard bundle contents."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Sequence

from fplinux_cli.artifacts.card_kit import CARD_APK_INDEX, CARD_APK_KEY
from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.paths import target_release_manifest_path
from fplinux_cli.manifests.values import exact_table, path_array


def load_release(target: str) -> dict[str, Any]:
    """Load the optional target overrides for publishing and documents."""
    path = target_release_manifest_path(target)
    if path.is_symlink() or (path.exists() and not path.is_file()):
        fail(f"target release manifest is invalid: {path}")
    overrides = load_toml(path) if path.is_file() else {}
    manifest = exact_table(
        {"publish": True, "documents": [], **overrides},
        {"publish", "documents"},
        f"target {target} release manifest",
    )
    publish = manifest["publish"]
    if not isinstance(publish, bool):
        fail("release manifest publish must be a boolean")
    return {
        "publish": publish,
        "image": "image/ramboot.bin",
        "documents": path_array(manifest.get("documents"), "release documents", allow_empty=True),
    }


def resolve_release(
    target: str,
    config: dict[str, Any],
    platform: dict[str, Any],
    *,
    bundle_packages: Sequence[str] = (),
) -> dict[str, Any]:
    """Derive the file inventory from the boot and package inputs."""
    release = load_release(target)
    bundle_files = [
        release["image"],
        *config["runtime"]["assets"].values(),
        *(f"host/{name}" for name in platform["host"]["runtime_tools"].values()),
        "runtime-manifest.json",
        "assets.lock.toml",
        "THIRD_PARTY_NOTICES.md",
    ]
    if bundle_packages:
        bundle_files.extend(f"apks/{package}.apk" for package in bundle_packages)
        bundle_files.extend((CARD_APK_INDEX, CARD_APK_KEY))
    return {**release, "bundle_files": path_array(bundle_files, "release bundle files")}

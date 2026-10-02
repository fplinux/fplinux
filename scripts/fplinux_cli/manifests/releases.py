# SPDX-License-Identifier: GPL-2.0-only
"""Load release contents and recorded runtime verification."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from collections.abc import Sequence

from fplinux_cli import common
from fplinux_cli.common import fail, load_toml
from fplinux_cli.manifests.paths import target_release_manifest_path
from fplinux_cli.manifests.values import exact_table, path_array, relative_value, sha256_value


def load_release(target: str, *, profile_packages: Sequence[str] = ()) -> dict[str, Any]:
    """Load release contents, omitting optional APKs preinstalled by the profile."""
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
    documents = path_array(manifest.get("documents"), "release documents")
    if image not in runtime_files:
        fail("release manifest image must be a runtime file")
    if not set(runtime_files).issubset(bundle_files):
        fail("release runtime files must be bundle files")
    preinstalled_apks = {f"apks/{package}.apk" for package in profile_packages}
    bundle_files = [
        relative
        for relative in bundle_files
        if relative not in preinstalled_apks or relative in runtime_files
    ]
    return {
        "image": image,
        "bundle_files": bundle_files,
        "runtime_files": runtime_files,
        "documents": documents,
    }


def verified_runtime_digest(target: str) -> str | None:
    """Return the recorded phone-tested runtime closure digest, if present."""
    path = common.ROOT / "releases.lock.toml"
    if not path.is_file():
        fail(f"release verification lock is missing: {path}")
    lock = load_toml(path)
    digest = lock.get("verified", {}).get(target)
    if digest is None:
        return None
    return sha256_value(digest, f"verified runtime SHA256 for {target} in {path}")

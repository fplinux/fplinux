# SPDX-License-Identifier: GPL-2.0-only
"""Keep the Alpine outputs needed by both build types."""

from __future__ import annotations

import io
import tarfile
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.alpine import packages, rootfs, rootfs_state, signing
from fplinux_cli.alpine.recipes import alpine_package_recipe, alpine_rootfs_recipe
from fplinux_cli.alpine.selection import aport_build_order, selected_packages
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.cache.prune.operations import (
    apply_prune,
    discard_obsolete_apks,
    discard_obsolete_rootfs,
)
from fplinux_cli.environment.definition import (
    environment_artifact_recipe_digest,
    environment_recipe_digest,
)
from fplinux_cli.environment.state import EnvironmentState, publish_environment_state
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from tests.small.cache.prune_fixtures import patch_prune_discovery

if TYPE_CHECKING:
    from pathlib import Path


def _publish_rootfs(cache: Path, recipe: str) -> Path:
    output = cache / "rootfs" / recipe
    output.mkdir(parents=True)
    (output / "rootfs.cpio").write_bytes(b"rootfs\n")
    (output / "initramfs.cpio").write_bytes(b"initramfs\n")
    rootfs_state.write_receipt(output, recipe)
    return output


def _publish_apk(cache: Path, name: str, environment_recipe: str, signing_key: str) -> None:
    slot = cache / "apks" / name
    slot.mkdir(parents=True)
    apk = slot / f"{name}-1-r0.apk"
    metadata = f"pkgname = {name}\n".encode()
    with tarfile.open(apk, "w:gz") as archive:
        info = tarfile.TarInfo(".PKGINFO")
        info.size = len(metadata)
        archive.addfile(info, io.BytesIO(metadata))
    recipe = alpine_package_recipe(name, environment_recipe, signing_key)
    packages._write_package_receipt(slot, recipe, [apk])  # noqa: SLF001 -- package producer.


@pytest.mark.parametrize("cleanup", ["after-build", "prune"])
def test_cleanup_retains_release_and_debug_rootfs_layers_and_apks(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, cleanup: str
) -> None:
    """Prepare current producer receipts, then verify retention and debug cache reuse."""
    cache = tmp_path / "data"
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "home"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "home"))
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(cache))
    public_key = signing.signing_public_key(cache)
    public_key.parent.mkdir(parents=True)
    public_key.write_bytes(b"test public key\n")
    public_key.with_suffix("").write_bytes(b"test private key\n")
    signing_key = signing.signing_key_identity(cache)
    source_recipe = environment_recipe_digest()
    publish_environment_state(cache, EnvironmentState(source_recipe, "a" * 64, "b" * 64))
    environment_recipe = environment_artifact_recipe_digest(source_recipe, "b" * 64)
    release = current_release_context()
    platform = load_platform("ums9117")
    retained: dict[str, Path] = {}
    packages_by_build_type: dict[str, tuple[str, ...]] = {}
    for build_type in ("release", "debug"):
        target = load_target("nokia-ta1618", build_type=build_type)
        base_packages = rootfs.shared_ram_packages(platform, target)
        assert base_packages is not None
        packages_by_build_type[build_type] = base_packages
        base_recipe = alpine_rootfs_recipe(
            environment_recipe, signing_key, base_packages, release=release
        )
        retained[f"{build_type} shared RAM rootfs"] = _publish_rootfs(cache, base_recipe)
        recipe = alpine_rootfs_recipe(
            environment_recipe,
            signing_key,
            selected_packages(platform, target),
            release=release,
            display_brightness=target.get("display_brightness"),
        )
        retained[f"{build_type} board RAM rootfs"] = _publish_rootfs(
            cache, rootfs_state.layered_rootfs_recipe(recipe, base_recipe)
        )
        card = load_target("nokia-ta1618", "microsd-uboot", build_type=build_type)
        card_packages = rootfs.shared_card_packages(platform, card)
        assert card_packages is not None
        card_recipe = alpine_rootfs_recipe(
            environment_recipe,
            signing_key,
            card_packages,
            release=release,
            root_kind="external",
        )
        retained[f"{build_type} shared microSD rootfs"] = _publish_rootfs(cache, card_recipe)

    debug_base_packages = packages_by_build_type["debug"]
    for name in aport_build_order(debug_base_packages):
        _publish_apk(cache, name, environment_recipe, signing_key)
    debug_apk = cache / "apks/fplinux-debug-tools/fplinux-debug-tools-1-r0.apk"
    stale_rootfs = _publish_rootfs(cache, "1" * 64)
    stale_apk = cache / "apks/fplinux-obsolete"
    stale_apk.mkdir()

    with patch_prune_discovery("discover_targets", return_value=("nokia-ta1618",)):
        if cleanup == "after-build":
            discard_obsolete_rootfs(cache)
            discard_obsolete_apks(cache)
        else:
            apply_prune(cache)

    assert not stale_rootfs.exists()
    assert not stale_apk.exists()
    expected = {
        **{label: path / "rootfs.cpio" for label, path in retained.items()},
        "fplinux-debug-tools APK": debug_apk,
    }
    missing = [label for label, path in expected.items() if not path.is_file()]
    assert missing == []
    for output in retained.values():
        assert (output / "rootfs.cpio").read_bytes() == b"rootfs\n"

    monkeypatch.setenv("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE", environment_recipe)
    monkeypatch.setattr(rootfs, "CACHE", cache)
    monkeypatch.setattr(packages, "CACHE", cache)
    monkeypatch.setattr(signing, "CACHE", cache)

    def unexpected_download(*_args: object, **_kwargs: object) -> None:
        pytest.fail("the second debug rootfs request should reuse the cached shared RAM layer")

    monkeypatch.setattr(rootfs, "acquire_source", unexpected_download)
    cached_rootfs, output, _recipe, _bundled = rootfs.build_rootfs(1, debug_base_packages)
    assert output == retained["debug shared RAM rootfs"]
    assert cached_rootfs.read_bytes() == b"rootfs\n"

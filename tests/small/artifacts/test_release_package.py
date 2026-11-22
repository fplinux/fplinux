# SPDX-License-Identifier: GPL-2.0-only
"""Small policy tests for release archive input validation."""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import registration
from fplinux_cli.alpine import selection as alpine_state
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.packaging import archive as package_commands

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path

DEMO_BUNDLE_FILES = [
    "image/ramboot.bin",
    "assets/fdl1.bin",
    "host/keyboard",
    "runtime-manifest.json",
    "assets.lock.toml",
    "THIRD_PARTY_NOTICES.md",
]


@dataclass(frozen=True)
class ReleaseInputs:
    """The release policy input tree and editable manifest records for one case."""

    root: Path
    target: str
    target_config: dict[str, Any]
    platform: dict[str, Any]
    release_manifest: dict[str, Any]
    readme: Path

    def write_manifest(self, manifest: dict[str, Any]) -> None:
        """Write target-owned release fields in the parser's TOML input."""
        (self.readme.parent / "manifest.toml").write_text(
            "\n".join(f"{key} = {json.dumps(value)}" for key, value in manifest.items()) + "\n",
            encoding="utf-8",
        )


@pytest.fixture
def release_inputs(tmp_path: Path) -> ReleaseInputs:
    """Create the minimal target tree accepted by the release manifest parser."""
    root = tmp_path
    target = "nokia-ta1618"
    target_config: dict[str, Any] = {
        "platform": "demo",
        "rootfs": {"base_packages": [], "packages": []},
        "bundle": {"packages": []},
        "runtime": {"assets": {"fdl1": "assets/fdl1.bin"}},
    }
    # The runtime uses "keyboard"; "extractor" is built for the checkout only.
    platform: dict[str, Any] = {
        "rootfs": {"packages": ["fplinux-font-terminus-6x12"]},
        "bundle": {"packages": ["demo"]},
        "host": {
            "runtime_tools": {"keyboard": "keyboard"},
            "tools": [{"name": "keyboard"}, {"name": "extractor"}],
        },
    }
    release_manifest: dict[str, Any] = {
        "documents": ["release/EXTRA.txt"],
    }
    readme = root / "targets" / target / "release/EXTRA.txt"
    readme.parent.mkdir(parents=True)
    readme.write_text("phone instructions\n", encoding="utf-8")
    for name in ("alpine.lock.toml", "sources.lock.toml"):
        (root / name).write_bytes((ROOT / "tests/fixtures/alpine" / name).read_bytes())
    for package in (*registration.COMMON_PACKAGES, "demo", "fplinux-font-terminus"):
        apkbuild = root / "alpine/aports" / package / "APKBUILD"
        apkbuild.parent.mkdir(parents=True)
        apkbuild.write_text(f"pkgname={package}\n", encoding="utf-8")
    return ReleaseInputs(
        root=root,
        target=target,
        target_config=target_config,
        platform=platform,
        release_manifest=release_manifest,
        readme=readme,
    )


class ReleaseManifestPolicyTests:
    """Exercise static release paths and tool selection without creating an archive."""

    @staticmethod
    @pytest.mark.parametrize(
        "target", ["inoi-240-modern-4g", "inoi-244-modern-4g", "nokia-ta1618"]
    )
    def test_supported_release_manifests_keep_curses_and_omit_unused_extensions(
        target: str,
    ) -> None:
        """Release manifests include optional curses and omit unused package extensions."""
        optional = {"fplinux-ncurses-curses"}
        omitted = {
            "fplinux-alsa-lib-card-profiles",
            "fplinux-bash-loadables",
        }
        target_config = targets.load_target(target)
        platform = platforms.load_platform(target_config["platform"])
        rootfs = alpine_state.selected_packages(platform, target_config)
        bundle = alpine_state.bundle_packages(platform, target_config, rootfs)
        manifest = releases.resolve_release(
            target, target_config, platform, bundle_packages=bundle
        )

        assert optional <= set(bundle)
        assert not (optional & set(rootfs))
        assert not (omitted & (set(rootfs) | set(bundle)))
        for package in optional:
            assert f"apks/{package}.apk" in manifest["bundle_files"]
        for package in omitted:
            assert f"apks/{package}.apk" not in manifest["bundle_files"]

    def test_target_release_paths_preserve_bytes_and_reject_symlinks(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """Release files keep their content and cannot traverse target-tree symlinks."""
        with mock.patch.object(common, "ROOT", release_inputs.root):
            readme_name, readme = package_commands.target_archive_file(
                release_inputs.target, "release/EXTRA.txt"
            )
            assert (readme_name) == ("EXTRA.txt")
            assert (readme.read_bytes()) == (b"phone instructions\n")

            with pytest.raises(SystemExit, match="invalid target package name"):
                package_commands.target_archive_file("../phone", "release/EXTRA.txt")

            link = release_inputs.readme.parent / "LINK.txt"
            link.symlink_to("EXTRA.txt")
            with pytest.raises(SystemExit, match="must not traverse a symlink"):
                package_commands.target_archive_file(release_inputs.target, "release/LINK.txt")

    @pytest.mark.parametrize(
        "relative",
        [
            "../release/EXTRA.txt",
            "features/MICROSD.md",
            "other/README.txt",
            "release",
        ],
        ids=["parent-path", "feature-page", "wrong-directory", "no-file-name"],
    )
    def test_target_document_paths_reject_invalid_inputs(
        self, release_inputs: ReleaseInputs, relative: str
    ) -> None:
        """Unsafe or unsupported document paths cannot become archive members."""
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            pytest.raises(SystemExit, match="target package file"),
        ):
            package_commands.target_archive_file(release_inputs.target, relative)

    def test_release_manifest_uses_selected_apks_and_keeps_target_runtime_and_documents(
        self,
        release_inputs: ReleaseInputs,
    ) -> None:
        """Selected APKs extend the target's release contents without changing its runtime."""
        release_inputs.write_manifest(release_inputs.release_manifest)
        with mock.patch.object(common, "ROOT", release_inputs.root):
            release = releases.resolve_release(
                release_inputs.target,
                release_inputs.target_config,
                release_inputs.platform,
                bundle_packages=("demo", "board-extra"),
            )
            no_optional_apks = releases.resolve_release(
                release_inputs.target, release_inputs.target_config, release_inputs.platform
            )

        assert release["bundle_files"] == [
            *DEMO_BUNDLE_FILES,
            "apks/demo.apk",
            "apks/board-extra.apk",
            "apks/Packages.adb",
            "apks/fplinux-build.rsa.pub",
        ]
        assert release["documents"] == ["release/EXTRA.txt"]
        assert no_optional_apks["bundle_files"] == DEMO_BUNDLE_FILES

    def test_archive_manifest_retains_preinstalled_applications_and_runtime_apks(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """Installing a default app keeps its APK and dependencies in the archive."""
        release_inputs.write_manifest(release_inputs.release_manifest)
        lock = {
            "runtime": {
                "packages": ["base-1-r0.apk"],
                "additions": {"demo": ["feature-runtime-2-r0.apk"]},
            }
        }
        profile = {
            **release_inputs.target_config,
            "applications": ["demo"],
        }

        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
            mock.patch.object(package_commands, "load_alpine_lock", return_value=lock),
        ):
            optional = package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )
            installed = package_commands.load_release_manifest(release_inputs.target, profile)

        expected_bundle_files = [
            *DEMO_BUNDLE_FILES,
            "apks/demo.apk",
            "apks/feature-runtime.apk",
            "apks/Packages.adb",
            "apks/fplinux-build.rsa.pub",
        ]
        for manifest in (optional, installed):
            assert manifest["bundle_files"] == expected_bundle_files
            assert manifest["documents"] == ["release/EXTRA.txt"]

    @pytest.mark.parametrize(
        ("field", "value"),
        [
            ("bundle_files", ["apks/demo.apk"]),
            ("image", "image/other.bin"),
        ],
    )
    def test_release_manifest_cannot_override_the_build_inventory(
        self, release_inputs: ReleaseInputs, field: str, value: str | list[str]
    ) -> None:
        """A target release cannot replace the bundle package owner's optional APK set."""
        release_inputs.write_manifest({**release_inputs.release_manifest, field: value})
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            pytest.raises(SystemExit, match="must contain exactly: documents, publish"),
        ):
            releases.load_release(release_inputs.target)

    def test_target_release_file_cannot_replace_the_shared_license(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """Target files cannot replace the required shared legal notice."""
        duplicate = release_inputs.readme.parent / "LICENSE"
        duplicate.write_bytes(b"different notice\n")
        release_inputs.write_manifest(
            {
                **release_inputs.release_manifest,
                "documents": ["release/EXTRA.txt", "release/LICENSE"],
            }
        )
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
            pytest.raises(SystemExit, match="duplicate release archive path"),
        ):
            package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )

    def test_release_manifest_rejects_a_target_feature_page(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """A feature page cannot restore separately maintained archive documentation."""
        feature = release_inputs.root / "targets" / release_inputs.target / "features/MICROSD.md"
        feature.parent.mkdir(parents=True)
        feature.write_bytes(b"phone microSD procedures\n")
        release_inputs.write_manifest(
            {
                **release_inputs.release_manifest,
                "documents": ["release/EXTRA.txt", "features/MICROSD.md"],
            }
        )
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
            pytest.raises(SystemExit, match="must be below release/"),
        ):
            package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )

    def test_release_without_a_target_manifest_uses_platform_inputs(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """A target gets the standard deliverables without repeating their file list."""
        with mock.patch.object(common, "ROOT", release_inputs.root):
            release = releases.resolve_release(
                release_inputs.target, release_inputs.target_config, release_inputs.platform
            )
        assert release["publish"]
        assert release["documents"] == []
        assert release["bundle_files"] == DEMO_BUNDLE_FILES

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

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class ReleaseInputs:
    """The release policy input tree and editable manifest records for one case."""

    root: Path
    target: str
    target_config: dict[str, Any]
    platform: dict[str, Any]
    release_manifest: dict[str, Any]
    readme: Path


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
        "image": "image/ramboot.bin",
        "bundle_files": [
            "image/ramboot.bin",
            "assets/fdl1.bin",
            "host/keyboard",
            "runtime-manifest.json",
            "apks/demo.apk",
            "assets.lock.toml",
        ],
        "runtime_files": [
            "image/ramboot.bin",
            "assets/fdl1.bin",
            "host/keyboard",
            "runtime-manifest.json",
        ],
        "documents": ["release/EXTRA.txt"],
    }
    readme = root / "targets" / target / "release/EXTRA.txt"
    readme.parent.mkdir(parents=True)
    readme.write_text("phone instructions\n", encoding="utf-8")
    for package in (*registration.COMMON_PACKAGES, "fplinux-font-terminus", "demo"):
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
        manifest = releases.load_release(target, bundle_packages=bundle)

        assert optional <= set(bundle)
        assert not (optional & set(rootfs))
        assert not (omitted & (set(rootfs) | set(bundle)))
        for package in optional:
            path = f"apks/{package}.apk"
            assert (path) in (manifest["bundle_files"])
            assert (path) not in (manifest["runtime_files"])
        for package in omitted:
            path = f"apks/{package}.apk"
            assert (path) not in (manifest["bundle_files"])
            assert (path) not in (manifest["runtime_files"])

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

    def test_profile_preinstall_omits_only_its_optional_archive_apk(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """Packaging does not require an optional APK already selected by the profile."""
        path = release_inputs.readme.parent / "manifest.toml"
        source_manifest = {
            **release_inputs.release_manifest,
            "bundle_files": [
                relative
                for relative in release_inputs.release_manifest["bundle_files"]
                if not relative.startswith("apks/")
            ],
        }
        path.write_text(
            "\n".join(f"{key} = {json.dumps(value)}" for key, value in source_manifest.items())
            + "\n",
            encoding="utf-8",
        )
        profile = {
            **release_inputs.target_config,
            "rootfs": {"base_packages": [], "packages": ["demo"]},
        }
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
        ):
            default = package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )
            preinstalled = package_commands.load_release_manifest(release_inputs.target, profile)

        assert ("apks/demo.apk") in (default["qualification_files"])
        assert ("apks/demo.apk") not in (preinstalled["qualification_files"])
        assert (preinstalled["bundle_files"]) == (
            [
                path
                for path in release_inputs.release_manifest["bundle_files"]
                if path != "apks/demo.apk"
            ]
        )
        assert (
            (preinstalled["runtime_files"]) == (release_inputs.release_manifest["runtime_files"])
        )
        assert (preinstalled["documents"]) == (release_inputs.release_manifest["documents"])

    def test_release_manifest_uses_selected_apks_and_keeps_target_runtime_and_documents(
        self,
        release_inputs: ReleaseInputs,
    ) -> None:
        """Selected APKs extend the target's release contents without changing its runtime."""
        source_manifest = {
            **release_inputs.release_manifest,
            "bundle_files": [
                relative
                for relative in release_inputs.release_manifest["bundle_files"]
                if not relative.startswith("apks/")
            ],
        }
        (release_inputs.readme.parent / "manifest.toml").write_text(
            "\n".join(f"{key} = {json.dumps(value)}" for key, value in source_manifest.items())
            + "\n",
            encoding="utf-8",
        )
        with mock.patch.object(common, "ROOT", release_inputs.root):
            release = releases.load_release(
                release_inputs.target, bundle_packages=("demo", "board-extra")
            )
            no_optional_apks = releases.load_release(release_inputs.target, bundle_packages=())

        assert release["bundle_files"] == [
            "image/ramboot.bin",
            "assets/fdl1.bin",
            "host/keyboard",
            "runtime-manifest.json",
            "assets.lock.toml",
            "apks/demo.apk",
            "apks/board-extra.apk",
        ]
        assert release["runtime_files"] == release_inputs.release_manifest["runtime_files"]
        assert release["documents"] == ["release/EXTRA.txt"]
        assert no_optional_apks["bundle_files"] == source_manifest["bundle_files"]

    def test_release_manifest_rejects_a_second_apk_inventory(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """A target release cannot replace the bundle package owner's optional APK set."""
        (release_inputs.readme.parent / "manifest.toml").write_text(
            "\n".join(
                f"{key} = {json.dumps(value)}"
                for key, value in release_inputs.release_manifest.items()
            )
            + "\n",
            encoding="utf-8",
        )
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            pytest.raises(SystemExit, match="resolved bundle package set"),
        ):
            releases.load_release(release_inputs.target, bundle_packages=("other",))

    def test_target_release_file_cannot_replace_the_shared_license(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """Target files cannot replace the required shared legal notice."""
        duplicate = release_inputs.readme.parent / "LICENSE"
        duplicate.write_bytes(b"different notice\n")
        manifest = {
            **release_inputs.release_manifest,
            "documents": [
                "release/EXTRA.txt",
                "release/LICENSE",
            ],
        }
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(releases, "load_release", return_value=manifest),
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
        manifest = {
            **release_inputs.release_manifest,
            "bundle_files": [
                relative
                for relative in release_inputs.release_manifest["bundle_files"]
                if not relative.startswith("apks/")
            ],
            "documents": ["release/EXTRA.txt", "features/MICROSD.md"],
        }
        (release_inputs.readme.parent / "manifest.toml").write_text(
            "\n".join(f"{key} = {json.dumps(value)}" for key, value in manifest.items()) + "\n",
            encoding="utf-8",
        )
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
            pytest.raises(SystemExit, match="must be below release/"),
        ):
            package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )

    def test_release_contract_requires_the_runtime_manifest(
        self, release_inputs: ReleaseInputs
    ) -> None:
        """The runtime manifest supplies the built closure independently of release files."""
        broken = {
            **release_inputs.release_manifest,
            "runtime_files": [
                path
                for path in release_inputs.release_manifest["runtime_files"]
                if path != "runtime-manifest.json"
            ],
        }
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(releases, "load_release", return_value=broken),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
            pytest.raises(SystemExit, match="omit required runtime inputs"),
        ):
            package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )

    def test_release_contract_requires_runner_tools(self, release_inputs: ReleaseInputs) -> None:
        """Release validation rejects a bundle missing a required runner tool."""
        without_runner_tool = {
            **release_inputs.release_manifest,
            "runtime_files": [
                path
                for path in release_inputs.release_manifest["runtime_files"]
                if path != "host/keyboard"
            ],
        }
        with (
            mock.patch.object(common, "ROOT", release_inputs.root),
            mock.patch.object(releases, "load_release", return_value=without_runner_tool),
            mock.patch.object(platforms, "load_platform", return_value=release_inputs.platform),
            pytest.raises(SystemExit, match="omit required runtime inputs"),
        ):
            package_commands.load_release_manifest(
                release_inputs.target, release_inputs.target_config
            )

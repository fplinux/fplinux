# SPDX-License-Identifier: GPL-2.0-only
"""Small policy tests for release archive input validation."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import selection as alpine_state
from fplinux_cli.cli import package as package_commands
from fplinux_cli.manifests import platforms, releases, targets

if TYPE_CHECKING:
    from pathlib import Path


class ReleaseManifestPolicyTests:
    """Exercise release-path and runtime-closure policy without creating an archive."""

    @pytest.fixture(autouse=True)
    def _release_inputs(self, tmp_path: Path) -> None:
        """Create the minimal target tree accepted by the release manifest parser."""
        self.root = tmp_path
        self.target = "nokia-ta1618"
        self.target_config = {
            "platform": "demo",
            "runtime": {"assets": {"pinmap": "assets/pinmap.bin"}},
        }
        # The runner uses "keyboard"; "extractor" is built for the checkout only.
        self.platform = {
            "host": {
                "runtime_tools": {"keyboard": "keyboard"},
                "tools": [{"name": "keyboard"}, {"name": "extractor"}],
            }
        }
        self.release_manifest = {
            "image": "image/ramboot.bin",
            "bundle_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/keyboard",
                "runner/run.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
                "runner/loader_events.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
                "apks/demo.apk",
                "assets.lock.toml",
            ],
            "runtime_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/keyboard",
                "runner/run.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
                "runner/loader_events.py",
                "runner/platform_adapter.py",
                "runtime-manifest.json",
            ],
            "documents": ["release/README.txt", "features/MICROSD.md"],
        }
        self.readme = self.root / "targets" / self.target / "release/README.txt"
        self.readme.parent.mkdir(parents=True)
        self.readme.write_text("phone instructions\n", encoding="utf-8")
        self.feature = self.root / "targets" / self.target / "features/MICROSD.md"
        self.feature.parent.mkdir(parents=True)
        self.feature.write_bytes(b"phone microSD procedures\n")

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
        manifest = releases.load_release(target)

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

    def test_target_document_paths_are_safe_and_collision_free(self) -> None:
        """Direct feature pages map once while invalid or escaping inputs are rejected."""
        with mock.patch.object(common, "ROOT", self.root):
            readme_name, readme = package_commands.target_archive_file(
                self.target, "release/README.txt"
            )
            assert (readme_name) == ("README.txt")
            assert (readme.read_bytes()) == (b"phone instructions\n")

            archive_name, source = package_commands.target_archive_file(
                self.target, "features/MICROSD.md"
            )
            assert (archive_name) == ("docs/target/MICROSD.md")
            assert (source.read_bytes()) == (b"phone microSD procedures\n")

            with pytest.raises(SystemExit, match="invalid target package name"):
                package_commands.target_archive_file("../phone", "features/MICROSD.md")

            link = self.root / "targets" / self.target / "features/LINK.md"
            link.symlink_to("MICROSD.md")
            with pytest.raises(SystemExit, match="must not traverse a symlink"):
                package_commands.target_archive_file(self.target, "features/LINK.md")

    @pytest.mark.parametrize(
        "relative",
        [
            "../features/MICROSD.md",
            "features/nested/MICROSD.md",
            "features/MICROSD.txt",
            "other/MICROSD.md",
        ],
        ids=["parent-path", "nested-feature", "wrong-extension", "wrong-directory"],
    )
    def test_target_document_paths_reject_invalid_inputs(self, relative: str) -> None:
        """Unsafe or unsupported document paths cannot become archive members."""
        with (
            mock.patch.object(common, "ROOT", self.root),
            pytest.raises(SystemExit, match="target package file"),
        ):
            package_commands.target_archive_file(self.target, relative)

    def test_profile_preinstall_omits_only_its_optional_archive_apk(self) -> None:
        """Packaging does not require an optional APK already selected by the profile."""
        path = self.readme.parent / "manifest.toml"
        path.write_text(
            "\n".join(
                f"{key} = {json.dumps(value)}" for key, value in self.release_manifest.items()
            )
            + "\n",
            encoding="utf-8",
        )
        profile = {
            **self.target_config,
            "rootfs": {"base_packages": [], "packages": ["demo"], "exclude_packages": []},
        }
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
        ):
            default = package_commands.load_release_manifest(self.target, self.target_config)
            preinstalled = package_commands.load_release_manifest(self.target, profile)

        assert ("apks/demo.apk") in (default["qualification_files"])
        assert ("apks/demo.apk") not in (preinstalled["qualification_files"])
        assert (preinstalled["bundle_files"]) == (
            [path for path in self.release_manifest["bundle_files"] if path != "apks/demo.apk"]
        )
        assert (preinstalled["runtime_files"]) == (self.release_manifest["runtime_files"])
        assert (preinstalled["documents"]) == (self.release_manifest["documents"])

    def test_duplicate_target_document_paths_are_rejected(self) -> None:
        """Two declared documents cannot silently publish the same archive member."""
        duplicate = self.root / "targets" / self.target / "release/docs/target/MICROSD.md"
        duplicate.parent.mkdir(parents=True)
        duplicate.write_bytes(b"duplicate\n")
        manifest = {
            **self.release_manifest,
            "documents": [
                "release/README.txt",
                "release/docs/target/MICROSD.md",
                "features/MICROSD.md",
            ],
        }
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(releases, "load_release", return_value=manifest),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
            pytest.raises(SystemExit, match="duplicate release archive path"),
        ):
            package_commands.load_release_manifest(self.target, self.target_config)

    def test_runtime_closure_requires_the_identity_helper(self) -> None:
        """A standalone runner without its identity helper is rejected before packaging."""
        broken = {
            **self.release_manifest,
            "runtime_files": [
                path
                for path in self.release_manifest["runtime_files"]
                if path != "runner/identity.py"
            ],
        }
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(releases, "load_release", return_value=broken),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
            pytest.raises(SystemExit, match="omit required runtime inputs"),
        ):
            package_commands.load_release_manifest(self.target, self.target_config)

    def test_normalized_manifest_includes_runner_tools_but_not_build_only_tools(self) -> None:
        """Manifest validation requires runner tools and omits build tools from executables."""
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(releases, "load_release", return_value=self.release_manifest),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
        ):
            release = package_commands.load_release_manifest(self.target, self.target_config)
        assert ("host/keyboard") in (release["executables"])
        assert ("host/extractor") not in (release["executables"])

        without_runner_tool = {
            **self.release_manifest,
            "runtime_files": [
                path for path in self.release_manifest["runtime_files"] if path != "host/keyboard"
            ],
        }
        with (
            mock.patch.object(common, "ROOT", self.root),
            mock.patch.object(releases, "load_release", return_value=without_runner_tool),
            mock.patch.object(platforms, "load_platform", return_value=self.platform),
            pytest.raises(SystemExit, match="omit required runtime inputs"),
        ):
            package_commands.load_release_manifest(self.target, self.target_config)

# SPDX-License-Identifier: GPL-2.0-only
"""Small policy tests for release archive input validation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import commands


class ReleaseManifestPolicyTests(unittest.TestCase):
    """Exercise release-path and runtime-closure policy without creating an archive."""

    def setUp(self) -> None:
        """Create the minimal target tree accepted by the release manifest parser."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.target = "nokia-ta1618"
        self.target_config = {
            "platform": "demo",
            "runtime": {"assets": {"pinmap": "assets/pinmap.bin"}},
        }
        self.platform = {"host": {"tools": [{"name": "keyboard"}]}}
        self.release_manifest = {
            "image": "image/ramboot.bin",
            "bundle_files": [
                "image/ramboot.bin",
                "assets/pinmap.bin",
                "host/keyboard",
                "runner/run.py",
                "runner/identity.py",
                "runner/ssh_transport.py",
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

    def test_target_document_paths_are_safe_and_collision_free(self) -> None:
        """Direct feature pages map once while invalid or escaping inputs are rejected."""
        with mock.patch.object(commands, "ROOT", self.root):
            readme_name, readme = commands.target_archive_file(self.target, "release/README.txt")
            self.assertEqual(readme_name, "README.txt")
            self.assertEqual(readme.read_bytes(), b"phone instructions\n")

            archive_name, source = commands.target_archive_file(self.target, "features/MICROSD.md")
            self.assertEqual(archive_name, "docs/target/MICROSD.md")
            self.assertEqual(source.read_bytes(), b"phone microSD procedures\n")

            profile_readme = self.root.joinpath(
                "targets",
                self.target,
                "profiles/microsd/release/README.txt",
            )
            profile_readme.parent.mkdir(parents=True)
            profile_readme.write_bytes(b"profile instructions\n")
            readme_name, readme = commands.target_archive_file(
                self.target, "release/README.txt", "microsd"
            )
            self.assertEqual(readme_name, "README.txt")
            self.assertEqual(readme, profile_readme)

            for relative in (
                "../features/MICROSD.md",
                "features/nested/MICROSD.md",
                "features/MICROSD.txt",
                "other/MICROSD.md",
            ):
                with (
                    self.subTest(relative=relative),
                    self.assertRaisesRegex(SystemExit, "target package file"),
                ):
                    commands.target_archive_file(self.target, relative)

            with self.assertRaisesRegex(SystemExit, "invalid target package name"):
                commands.target_archive_file("../phone", "features/MICROSD.md")

            link = self.root / "targets" / self.target / "features/LINK.md"
            link.symlink_to("MICROSD.md")
            with self.assertRaisesRegex(SystemExit, "must not traverse a symlink"):
                commands.target_archive_file(self.target, "features/LINK.md")

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
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_release", return_value=manifest),
            mock.patch.object(commands, "load_platform", return_value=self.platform),
            self.assertRaisesRegex(SystemExit, "duplicate release archive path"),
        ):
            commands.load_release_manifest(self.target, self.target_config)

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
            mock.patch.object(commands, "ROOT", self.root),
            mock.patch.object(commands, "load_release", return_value=broken),
            mock.patch.object(commands, "load_platform", return_value=self.platform),
            self.assertRaisesRegex(SystemExit, "omit required runtime inputs"),
        ):
            commands.load_release_manifest(self.target, self.target_config)


if __name__ == "__main__":
    unittest.main()

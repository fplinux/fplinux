# SPDX-License-Identifier: GPL-2.0-only
"""Tests for source inventory policy boundaries."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check as source_check
from fplinux_cli import alpine_state
from fplinux_cli import workspace as workspace_module


class SourceInventoryTests(unittest.TestCase):
    """Keep scoped checks independent from unrelated source policy failures."""

    def test_non_source_inventory_skips_unrelated_artifacts(self) -> None:
        """Discover relevant text without enforcing the source-wide policy."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            markdown = root / "document.md"
            markdown.write_text("# Document\n")
            (root / "artifact.bin").write_bytes(b"\x00\xff")
            (root / "tool").write_bytes(b"\xff\xfe")
            (root / "linked.md").symlink_to(markdown)
            with mock.patch.object(source_check, "ROOT", root):
                files = source_check.source_files(enforce_policy=False)
                _python, discovered_markdown, posix_shell, bash = source_check.quality_sources(
                    files
                )
            self.assertEqual(files, [markdown, root / "tool"])
            self.assertEqual(discovered_markdown, ["document.md"])
            self.assertEqual(posix_shell, [])
            self.assertEqual(bash, [])

    def test_openrc_scripts_are_classified_as_posix_shell_sources(self) -> None:
        """Classify OpenRC init scripts as declared POSIX shell sources."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            initd = root / "service.initd"
            initd.write_text("#!/sbin/openrc-run\ncommand=/bin/true\n")
            with mock.patch.object(source_check, "ROOT", root):
                _python, _markdown, posix_shell, bash = source_check.quality_sources([initd])
            self.assertEqual(posix_shell, ["service.initd"])
            self.assertEqual(bash, [])

    def test_quality_workspace_skips_symlinks_outside_source_scope(self) -> None:
        """Keep host workspace staging aligned with scoped source policy."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            regular = root / "document.md"
            regular.write_text("# Document\n")
            (root / "linked.md").symlink_to(regular)
            inventory = subprocess.CompletedProcess(
                ["git", "ls-files"],
                0,
                b"document.md\0linked.md\0",
                b"",
            )
            with (
                mock.patch.object(workspace_module, "ROOT", root),
                mock.patch(
                    "fplinux_cli.workspace.subprocess.run",
                    return_value=inventory,
                ),
            ):
                self.assertEqual(
                    workspace_module.quality_files(enforce_source_policy=False),
                    [("document.md", regular)],
                )
                with self.assertRaisesRegex(SystemExit, "quality input must not be a symlink"):
                    workspace_module.quality_files(enforce_source_policy=True)

    def test_source_inventory_rejects_quake_game_data(self) -> None:
        """Keep PAK data outside source and generated images."""
        for name in ("pak0.pak", "pak0.part.00"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / name).write_text("game data\n")
                with (
                    mock.patch.object(source_check, "ROOT", root),
                    self.assertRaisesRegex(
                        SystemExit,
                        "Quake game data is not allowed",
                    ),
                ):
                    source_check.source_files(enforce_policy=True)

    def test_source_inventory_rejects_binary_artifacts(self) -> None:
        """Keep binary rejection in the explicit source scope."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "artifact.bin").write_bytes(b"\x00\xff")
            with (
                mock.patch.object(source_check, "ROOT", root),
                self.assertRaisesRegex(
                    SystemExit,
                    "binary artifact is not allowed",
                ),
            ):
                source_check.source_files(enforce_policy=True)

    def test_embedded_marker_changes_c_source_classification(self) -> None:
        """Keep embedded package C out of the standalone-analysis inventory."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "alpine/aports/demo-consumer"
            package.mkdir(parents=True)
            standalone = package / "app.c"
            embedded = package / "backend.c"
            standalone.write_text("int app;\n")
            embedded.write_text("/* fplinux-check: package-embedded */\nint backend;\n")
            files = [standalone, embedded]
            with (
                mock.patch.object(source_check, "ROOT", root),
                mock.patch.object(source_check, "discover_targets", return_value=()),
            ):
                self.assertEqual(
                    source_check.userspace_c_sources(files),
                    [("alpine/aports/demo-consumer/app.c", False)],
                )
                self.assertEqual(
                    source_check.userspace_c_sources(files, include_embedded=True),
                    [
                        ("alpine/aports/demo-consumer/app.c", False),
                        ("alpine/aports/demo-consumer/backend.c", False),
                    ],
                )

    def test_package_selection_validation_rejects_an_unresolved_target(self) -> None:
        """A missing declared aport makes the repository selection gate fail."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for package in ("common-runtime", "platform-app", "phone-ui"):
                aport = root / "alpine/aports" / package
                aport.mkdir(parents=True)
                (aport / "APKBUILD").write_text(f"pkgname={package}\n")
            targets = {
                "phone-a": {
                    "platform": "soc",
                    "bundle": {"packages": ["phone-ui"]},
                    "rootfs": {"packages": []},
                },
                "phone-b": {
                    "platform": "soc",
                    "bundle": {"packages": []},
                    "rootfs": {"packages": ["missing-board-app"]},
                },
            }
            platform = {
                "rootfs": {"packages": ["platform-app"]},
                "bundle": {"packages": []},
            }
            with (
                mock.patch.object(source_check, "ROOT", root),
                mock.patch.object(source_check, "discover_targets", return_value=tuple(targets)),
                mock.patch.object(source_check, "load_target", side_effect=targets.__getitem__),
                mock.patch.object(source_check, "load_platform", return_value=platform),
                mock.patch.object(alpine_state, "COMMON_PACKAGES", ("common-runtime",)),
                self.assertRaisesRegex(
                    SystemExit,
                    "selected aport is missing or invalid: missing-board-app",
                ),
            ):
                source_check.validate_package_selections()


if __name__ == "__main__":
    unittest.main()

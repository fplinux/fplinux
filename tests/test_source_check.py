# SPDX-License-Identifier: GPL-2.0-only
"""Tests for source inventory policy boundaries."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import check as source_check
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

    def test_quality_workspace_skips_symlinks_outside_source_scope(self) -> None:
        """Keep host workspace staging aligned with scoped source policy."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            regular = root / "document.md"
            regular.write_text("# Document\n")
            (root / "linked.md").symlink_to(regular)
            with mock.patch.object(workspace_module, "ROOT", root):
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


if __name__ == "__main__":
    unittest.main()

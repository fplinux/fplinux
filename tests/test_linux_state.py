# SPDX-License-Identifier: GPL-2.0-only
"""Regression tests for prepared Linux recipe receipts."""

from __future__ import annotations

import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import builder, linux_state


class PreparedLinuxTests(unittest.TestCase):
    """A prepared tree is reusable only for the exact integration recipe."""

    archive = "b" * 64
    recipe_a = "a" * 64
    recipe_b = "c" * 64

    def setUp(self) -> None:
        """Create an isolated Linux source fixture."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def _tree(self, name: str) -> Path:
        source = self.root / name
        (source / "drivers").mkdir(parents=True)
        (source / "drivers/config").write_text("upstream\n", encoding="utf-8")
        return source

    def _seal(self, source: Path, recipe: str) -> linux_state.PreparedLinuxState:
        return linux_state.seal_prepared_linux(source, recipe)

    def test_matching_marker_and_receipt_are_a_hit(self) -> None:
        """Accept matching marker and receipt identities."""
        source = self._tree("linux")
        state = self._seal(source, self.recipe_a)

        hit = linux_state.inspect_prepared_linux(source, self.recipe_a)

        self.assertEqual(hit, state)
        self.assertEqual(linux_state.inspect_prepared_linux(source, self.recipe_a), state)

    def test_recipe_change_rebuilds_and_replaces_the_prepared_tree(self) -> None:
        """Replace a prepared tree when its recipe changes."""
        cache = self.root / "cache"
        archive_root = self.root / "archive/linux-test"
        archive_root.mkdir(parents=True)
        (archive_root / "Makefile").write_text("VERSION = test\n", encoding="utf-8")
        archive = self.root / "linux-test.tar.xz"
        with tarfile.open(archive, "w:xz") as output:
            output.add(archive_root, arcname="linux-test")

        project = self.root / "project"
        project.mkdir()
        copied = project / "copied"
        copied.write_text("first\n", encoding="utf-8")
        sources = {
            "linux": {
                "version": "test",
                "url": "https://example.invalid/linux-test.tar.xz",
                "sha256": self.archive,
            }
        }
        target_config = {
            "linux": {
                "patches": [],
                "copies": [{"source": "copied", "destination": "generated"}],
                "appends": [],
            }
        }
        platform = {
            "linux": {
                "source_lock": "linux",
                "patches": [],
                "copies": [],
                "appends": [],
            }
        }

        with (
            mock.patch.object(builder, "CACHE", cache),
            mock.patch.object(
                builder,
                "target_source",
                side_effect=lambda _target, relative: project / relative,
            ),
            mock.patch.object(builder, "fetch", return_value=archive),
        ):
            source, first = builder.prepare_linux(sources, "demo", target_config, platform)
            (source / "untracked").write_text("old tree only\n", encoding="utf-8")
            copied.write_text("second\n", encoding="utf-8")
            rebuilt, second = builder.prepare_linux(sources, "demo", target_config, platform)

        self.assertEqual(rebuilt, source)
        self.assertNotEqual(second.linux_recipe, first.linux_recipe)
        self.assertEqual((source / "generated").read_text(encoding="utf-8"), "second\n")
        self.assertFalse((source / "untracked").exists())

    def test_tampered_receipt_is_rejected_before_a_consumer_uses_the_tree(self) -> None:
        """Reject a prepared tree if its sealed recipe receipt changes."""
        source = self._tree("linux")
        state = self._seal(source, self.recipe_a)
        (source / linux_state.RECEIPT_NAME).write_text(
            '{"linux_recipe":"' + self.recipe_b + '"}\n', encoding="utf-8"
        )

        self.assertIsNone(linux_state.inspect_prepared_linux(source, self.recipe_a))
        with self.assertRaisesRegex(
            linux_state.LinuxStateError, "prepared Linux tree changed after preparation"
        ):
            linux_state.require_prepared_linux(source, state)


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for prepared Linux recipe receipts."""

from __future__ import annotations

import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import builder, linux_state


class PreparedLinuxTests(unittest.TestCase):
    """A prepared tree is reusable only for its exact preparation recipe."""

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

    def test_recipe_change_reprepares_and_replaces_the_source_tree(self) -> None:
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

    def test_source_changing_profile_uses_a_sibling_not_the_default_tree(self) -> None:
        """A profile projection never creates state inside the sealed default source tree."""
        parent = self.root / "cache/linux/sources"
        parent.mkdir(parents=True)

        source = builder.profile_linux_source_path(parent, "phone", "host")

        self.assertEqual(source, self.root / "cache/linux/profiles/phone/host")
        self.assertFalse((parent / "phone/profiles").exists())

    def test_profile_reusing_default_sources_discards_its_old_dedicated_tree(self) -> None:
        """A patch-profile transition to Kconfig-only self-heals before a source hit."""
        cache = self.root / "cache"
        parent = cache / "linux/sources"
        parent.mkdir(parents=True)
        stale = cache / "linux/profiles/phone/host"
        stale.mkdir(parents=True)
        (stale / "old-projection").write_text("old\n", encoding="utf-8")
        recipe = "a" * 64
        linux = {"version": "test", "sha256": "b" * 64}
        target = {
            "profile": "host",
            "linux": {"patches": [], "copies": [], "appends": []},
        }
        platform = {"linux": {"source_lock": "linux", "patches": [], "copies": [], "appends": []}}

        with (
            mock.patch.object(builder, "CACHE", cache),
            mock.patch.object(builder, "load_target", return_value={"linux": target["linux"]}),
            mock.patch.object(builder, "linux_recipe_digest", return_value=recipe),
            mock.patch.object(
                linux_state,
                "inspect_prepared_linux",
                return_value=linux_state.PreparedLinuxState(recipe),
            ),
        ):
            source, prepared = builder.prepare_linux({"linux": linux}, "phone", target, platform)

        self.assertEqual(source, parent / "phone")
        self.assertEqual(prepared.linux_recipe, recipe)
        self.assertFalse(stale.exists())

    def test_staging_slot_self_heals_one_old_directory_and_is_discardable(self) -> None:
        """One fixed extraction slot replaces random prepare generations after interruption."""
        parent = self.root / "cache/linux/sources"
        parent.mkdir(parents=True)
        stale = self.root / "cache/linux/staging/phone/profiles/host"
        stale.mkdir(parents=True)
        (stale / "partial").write_text("partial\n", encoding="utf-8")

        staging = builder.prepared_linux_staging_path(parent, "phone", "host")

        self.assertEqual(staging, stale)
        self.assertEqual(tuple(staging.iterdir()), ())
        builder.discard_prepared_linux_staging(staging)
        self.assertFalse(staging.exists())

    def test_profile_source_cleanup_rejects_each_intermediate_symlink(self) -> None:
        """Profile source self-healing cannot traverse external cache path components."""
        parent = self.root / "cache/linux/sources"
        parent.mkdir(parents=True)
        external = self.root / "external"
        external.mkdir()
        sentinel = external / "sentinel"
        sentinel.write_text("keep\n", encoding="utf-8")
        profiles = self.root / "cache/linux/profiles"
        profiles.symlink_to(external, target_is_directory=True)

        with self.assertRaisesRegex(SystemExit, "source root must not be a symlink"):
            builder.discard_profile_linux_source(parent, "phone", "host")
        self.assertTrue(sentinel.exists())

        profiles.unlink()
        profiles.mkdir()
        (profiles / "phone").symlink_to(external, target_is_directory=True)
        with self.assertRaisesRegex(SystemExit, "target slot must not be a symlink"):
            builder.discard_profile_linux_source(parent, "phone", "host")
        self.assertTrue(sentinel.exists())

    def test_prepared_linux_cache_refuses_a_symlinked_sources_component(self) -> None:
        """The common preparer cannot publish through an external sources symlink."""
        cache = self.root / "cache"
        linux = cache / "linux"
        linux.mkdir(parents=True)
        external = self.root / "external"
        external.mkdir()
        sentinel = external / "sentinel"
        sentinel.write_text("keep\n", encoding="utf-8")
        (linux / "sources").symlink_to(external, target_is_directory=True)

        with self.assertRaisesRegex(linux_state.LinuxStateError, "cache directory"):
            linux_state.ensure_sources_directory(cache)
        self.assertTrue(sentinel.exists())


if __name__ == "__main__":
    unittest.main()

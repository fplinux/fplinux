# SPDX-License-Identifier: GPL-2.0-only
"""Linux source archive retention and staging cleanup."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import fplinux_cli.cache.prune.builds as prune_builds
from fplinux_cli import common
from fplinux_cli.cache.prune.operations import apply_prune, plan_prune


def _linux_declarations(root: Path) -> None:
    """Declare two independent platform archives and an unused source lock."""
    (root / "sources.lock.toml").write_text(
        f'[linux_a]\nsha256 = "{"a" * 64}"\n'
        f'[linux_b]\nsha256 = "{"b" * 64}"\n'
        f'[retired]\nsha256 = "{"c" * 64}"\n'
    )
    for platform, source in (("first", "linux_a"), ("second", "linux_b")):
        directory = root / "platforms" / platform
        directory.mkdir(parents=True)
        (directory / "platform.toml").write_text(f'[linux]\nsource_lock = "{source}"\n')


def _linux_archive_slots(cache: Path, digest: str) -> tuple[Path, Path]:
    """Create the source tree and sparse originals owned by one archive."""
    source = cache / "linux/sources" / digest
    originals = cache / "linux/originals" / digest
    for path in (source, originals):
        path.mkdir(parents=True)
        (path / ".fplinux-base").write_text(digest + "\n")
    (source / "Makefile").write_text("upstream tree\n")
    (originals / "files").mkdir()
    (originals / "files/Makefile").write_text("upstream tree\n")
    (originals / "index.json").write_text('{"Makefile": true}\n')
    return source, originals


class LinuxArchivePruneTests(unittest.TestCase):
    """Exercise prune planning and application on isolated cache trees."""

    def test_linux_archive_prune_preserves_every_current_platform_base(self) -> None:
        """Only an archive no longer selected by any platform becomes disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / ".cache"
            _linux_declarations(root)
            current = (
                *_linux_archive_slots(cache, "a" * 64),
                *_linux_archive_slots(cache, "b" * 64),
            )
            retired = _linux_archive_slots(cache, "c" * 64)
            with (
                mock.patch.object(prune_builds, "ROOT", root),
                mock.patch.object(common, "ROOT", root),
            ):
                plan = plan_prune(cache)
                result = apply_prune(cache)

            decisions = {entry.path: entry.action for entry in plan.entries}
            for path in current:
                self.assertEqual(decisions[path.relative_to(cache).as_posix()], "protected")
                self.assertTrue(path.is_dir())
            self.assertEqual(
                result.removed,
                (f"linux/originals/{'c' * 64}", f"linux/sources/{'c' * 64}"),
            )
            self.assertTrue(all(not path.exists() for path in retired))

    def test_unavailable_linux_source_declarations_protect_existing_bases(self) -> None:
        """An unresolved source lock cannot make a managed Linux archive disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / ".cache"
            _linux_declarations(root)
            (root / "platforms/second/platform.toml").write_text(
                '[linux]\nsource_lock = "missing"\n'
            )
            paths = _linux_archive_slots(cache, "c" * 64)
            with (
                mock.patch.object(prune_builds, "ROOT", root),
                mock.patch.object(common, "ROOT", root),
            ):
                plan = plan_prune(cache)
                result = apply_prune(cache)

            self.assertEqual(result.removed, ())
            self.assertTrue(all(entry.action == "protected" for entry in plan.entries))
            self.assertTrue(all(path.is_dir() for path in paths))

    def test_linux_prune_preserves_unowned_directories(self) -> None:
        """A SHA-shaped name alone does not claim source trees or originals."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / ".cache"
            _linux_declarations(root)
            paths = (
                cache / "linux/sources" / ("d" * 64),
                cache / "linux/originals" / ("d" * 64),
                cache / "linux/sources/manual",
                cache / "linux/staging/manual",
            )
            for path in paths:
                path.mkdir(parents=True)
                (path / "notes").write_text("keep\n")
            with (
                mock.patch.object(prune_builds, "ROOT", root),
                mock.patch.object(common, "ROOT", root),
            ):
                result = apply_prune(cache)

            self.assertEqual(result.removed, ())
            self.assertTrue(all((path / "notes").read_text() == "keep\n" for path in paths))

    def test_fixed_linux_staging_slots_are_disposable(self) -> None:
        """An extraction slot is disposable even while its archive remains current."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / ".cache"
            _linux_declarations(root)
            current = cache / "linux/staging" / ("a" * 64)
            retired = cache / "linux/staging" / ("c" * 64)
            for path in (current, retired):
                path.mkdir(parents=True)
                (path / "partial").write_text("partial\n")
            with (
                mock.patch.object(prune_builds, "ROOT", root),
                mock.patch.object(common, "ROOT", root),
            ):
                result = apply_prune(cache)

            self.assertEqual(
                result.removed,
                (f"linux/staging/{'a' * 64}", f"linux/staging/{'c' * 64}"),
            )
            self.assertFalse(current.exists())
            self.assertFalse(retired.exists())


if __name__ == "__main__":
    unittest.main()

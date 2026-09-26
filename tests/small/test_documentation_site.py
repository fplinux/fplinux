# SPDX-License-Identifier: GPL-2.0-only
"""Tests for collecting the documentation site sources and checking its navigation."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import check as source_check
import site_collect


def write_files(root: Path, files: dict[str, str]) -> None:
    """Create each root-relative text file with its parent directories."""
    for relative, text in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)


class SiteCollectionTests(unittest.TestCase):
    """Publish documentation at its repository paths and nothing else."""

    def test_collection_mirrors_documentation_paths_and_drops_stale_files(self) -> None:
        """Keep relative links valid, omit other sources and remove earlier pages."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "checkout"
            destination = Path(temporary) / "site"
            write_files(
                root,
                {
                    "README.md": "# Project\n",
                    "docs/guides/GUIDE.md": "# Guide\n",
                    "targets/phone/README.md": "# Phone\n",
                    "targets/phone/target.toml": "name = 'phone'\n",
                    "scripts/tool.py": "print()\n",
                },
            )
            write_files(destination, {"docs/REMOVED.md": "# Removed\n"})

            site_collect.collect(root, destination)

            collected = {
                path.relative_to(destination).as_posix(): path.read_text()
                for path in destination.rglob("*")
                if path.is_file()
            }
            self.assertEqual(
                collected,
                {
                    "README.md": "# Project\n",
                    "docs/guides/GUIDE.md": "# Guide\n",
                    "targets/phone/README.md": "# Phone\n",
                },
            )


class SiteNavigationTests(unittest.TestCase):
    """Require every published page to have a deliberate place in the site navigation."""

    def test_page_outside_nav_is_rejected_until_listed_or_marked_not_in_nav(self) -> None:
        """Name only the unplaced Markdown page and accept either explicit placement."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_files(
                root,
                {
                    "README.md": "# Project\n",
                    "LICENSE": "License text\n",
                    "docs/guides/GUIDE.md": "# Guide\n",
                    "docs/notes/DRAFT.md": "# Draft\n",
                },
            )
            config = root / "mkdocs.yml"
            nav = "nav:\n  - Home: README.md\n  - Guides:\n      - docs/guides/GUIDE.md\n"

            config.write_text(nav)
            with self.assertRaisesRegex(SystemExit, r"mkdocs\.yml nav: docs/notes/DRAFT\.md$"):
                source_check.check_site_navigation(root)

            config.write_text(nav + "  - Draft: docs/notes/DRAFT.md\n")
            source_check.check_site_navigation(root)

            config.write_text(nav + "not_in_nav: |\n  /docs/notes/\n")
            source_check.check_site_navigation(root)


if __name__ == "__main__":
    unittest.main()

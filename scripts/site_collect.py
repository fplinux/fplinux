#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Collect the documentation site sources into `.cache/site/src`."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE_SOURCES = Path(".cache/site/src")

# Every file keeps its repository path, so relative links between pages resolve
# on the site exactly as they do in the repository.
CORPUS_PATTERNS = (
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "LICENSE",
    "alpine/README.md",
    "bootstrap/README.md",
    "common/README.md",
    "docs/**/*",
    "platforms/**/README.md",
    "targets/**/README.md",
    "include/fplinux/fplinux-cli.h",
)


def corpus_files(root: Path) -> list[Path]:
    """Return the root-relative files published on the documentation site."""
    files = {
        path.relative_to(root)
        for pattern in CORPUS_PATTERNS
        for path in root.glob(pattern)
        if path.is_file()
    }
    return sorted(files)


def collect(root: Path, destination: Path) -> int:
    """Replace `destination` with a copy of the corpus and return its file count."""
    if destination.exists():
        shutil.rmtree(destination)
    files = corpus_files(root)
    for relative in files:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / relative, target)
    return len(files)


def main() -> None:
    # MkDocs reads mkdocs.yml from the current directory, so collection and
    # the site commands that follow it must run from the same checkout root.
    if Path.cwd().resolve() != ROOT:
        sys.exit(f"site_collect: run this from the repository root: {ROOT}")
    count = collect(ROOT, ROOT / SITE_SOURCES)
    print(f"site sources: {count} files in {SITE_SOURCES}")


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-2.0-only
"""Install pinned Node and npm and seed npm's cache from verified archives."""

from __future__ import annotations

import shutil
import subprocess
from typing import TYPE_CHECKING

from .archives import extract_archive

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path


def install_node_runtime(node_archive: Path, npm_archive: Path, destination: Path) -> None:
    """Install one pinned Node archive with the npm original selected by its consumer."""
    extract_archive(node_archive, destination, strip_components=1)
    npm_root = destination / "lib/node_modules/npm"
    shutil.rmtree(npm_root)
    extract_archive(npm_archive, npm_root, strip_components=1)
    for executable, script in (("npm", "npm-cli.js"), ("npx", "npx-cli.js")):
        link = destination / "bin" / executable
        link.unlink(missing_ok=True)
        link.symlink_to(f"../lib/node_modules/npm/bin/{script}")


def populate_npm_cache(npm: Path, cache: Path, archives: Sequence[Path]) -> None:
    """Import verified registry originals through npm's supported offline cache boundary."""
    for archive in archives:
        subprocess.run(
            [
                npm.with_name("node"),
                npm,
                "cache",
                "add",
                "--offline",
                "--ignore-scripts",
                "--cache",
                cache,
                archive,
            ],
            check=True,
        )

# SPDX-License-Identifier: GPL-2.0-only
"""Publish prepared host packages and their shared toolchain licenses."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Iterable
    from pathlib import Path


def publish_host_package(
    package: Path, destination: Path, *, relative_paths: Iterable[str]
) -> dict[str, Path]:
    """Replace the previous package and return paths that survive preparation cleanup."""
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(package, destination)
    return {relative: destination / relative for relative in relative_paths}


def copy_mingw_licenses(toolchain: Path, licenses: Path) -> None:
    """Copy the compiler and runtime notices into an existing license directory."""
    for name in ("COPYING", "COPYING.MinGW-w64-runtime.txt", "COPYING.MinGW-w64.txt"):
        shutil.copyfile(
            toolchain / "x86_64-w64-mingw32/share/mingw32" / name,
            licenses / f"mingw-w64-{name}",
        )
    shutil.copyfile(toolchain / "LICENSE.TXT", licenses / "llvm-LICENSE.txt")

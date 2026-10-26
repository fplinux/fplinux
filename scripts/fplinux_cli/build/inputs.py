# SPDX-License-Identifier: GPL-2.0-only
"""Resolve validated build inputs and output locations."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.values import relative_value, sha256_value

CACHE = Path("/cache")
OUTPUT = Path("/out")

_CORE_IMPLEMENTATION_SOURCES = (
    "scripts/fplinux_cli/common.py",
    "scripts/fplinux_cli/templates.py",
    "scripts/fplinux_cli/build/environment.py",
    "scripts/fplinux_cli/build/inputs.py",
    "scripts/fplinux_cli/build/process.py",
    "scripts/fplinux_cli/build/sources.py",
    "scripts/fplinux_cli/environment/downloads.py",
    "scripts/fplinux_cli/manifests/source_catalog.py",
    "scripts/fplinux_cli/manifests/values.py",
)
_BOOTSTRAP_IMPLEMENTATION_SOURCES = (
    *_CORE_IMPLEMENTATION_SOURCES,
    "scripts/fplinux_cli/build/display.py",
    "scripts/fplinux_cli/build/identity.py",
    "scripts/fplinux_cli/build/bootstrap/recipe.py",
    "scripts/fplinux_cli/build/bootstrap/stage.py",
    "scripts/fplinux_cli/build/storage/layout.py",
    "scripts/fplinux_cli/manifests/identity.py",
    "scripts/fplinux_cli/manifests/display.py",
)
_SC9820E_BOOTSTRAP_IMPLEMENTATION_SOURCES = (
    "scripts/fplinux_cli/common.py",
    "scripts/fplinux_cli/templates.py",
    "scripts/fplinux_cli/build/environment.py",
    "scripts/fplinux_cli/build/inputs.py",
    "scripts/fplinux_cli/build/process.py",
    "scripts/fplinux_cli/build/device_tree.py",
    "scripts/fplinux_cli/build/bootstrap/recipe.py",
    "scripts/fplinux_cli/build/bootstrap/stage.py",
    "scripts/fplinux_cli/build/bootstrap/result.py",
    "scripts/fplinux_cli/build/bootstrap/sc9820e.py",
    "scripts/fplinux_cli/build/bootstrap/verify.py",
)
_KERNEL_IMPLEMENTATION_SOURCES = (
    *_CORE_IMPLEMENTATION_SOURCES,
    "scripts/fplinux_cli/build/display.py",
    "scripts/fplinux_cli/build/device_tree.py",
    "scripts/fplinux_cli/build/identity.py",
    "scripts/fplinux_cli/build/machine-binding.txt.in",
    "scripts/fplinux_cli/build/kernel/prepare.py",
    "scripts/fplinux_cli/build/kernel/configuration.py",
    "scripts/fplinux_cli/build/kernel/compile.py",
    "scripts/fplinux_cli/build/kernel/identity.py",
    "scripts/fplinux_cli/build/kernel/projection.py",
    "scripts/fplinux_cli/build/kernel/state.py",
    "scripts/fplinux_cli/build/kernel/receipts.py",
    "scripts/fplinux_cli/build/storage/layout.py",
    "scripts/fplinux_cli/device_data/inputs.py",
    "scripts/fplinux_cli/manifests/identity.py",
    "scripts/fplinux_cli/manifests/audio.py",
    "scripts/fplinux_cli/manifests/display.py",
    "scripts/fplinux_cli/manifests/kernel.py",
    "scripts/fplinux_cli/manifests/linux.py",
)
_BUILD_STAGE_SOURCES = (
    "scripts/fplinux_cli/build/__init__.py",
    "scripts/fplinux_cli/build/__main__.py",
    "scripts/fplinux_cli/build/assets.py",
    "scripts/fplinux_cli/build/host_tools/__init__.py",
    "scripts/fplinux_cli/build/host_tools/host.py",
    "scripts/fplinux_cli/build/publish.py",
    "scripts/fplinux_cli/build/runtime.py",
    "scripts/fplinux_cli/build/bootstrap/__init__.py",
    "scripts/fplinux_cli/build/bootstrap/verify.py",
    "scripts/fplinux_cli/build/kernel/__init__.py",
    "scripts/fplinux_cli/build/storage/__init__.py",
    "scripts/fplinux_cli/build/storage/stage.py",
    "scripts/fplinux_cli/device_data/cpio.py",
)


def require_file(path: Path) -> Path:
    """Require a regular, non-symlink file."""
    if path.is_symlink() or not path.is_file():
        fail(f"expected file is missing or invalid: {path}")
    return path


def require_directory(path: Path) -> Path:
    """Require a directory that is not a symlink."""
    if path.is_symlink() or not path.is_dir():
        fail(f"expected directory is missing or invalid: {path}")
    return path


def environment_identity() -> tuple[str, str]:
    """Validate the declared recipe and actual installed content supplied by the host."""
    environment_recipe = sha256_value(
        os.environ.get("FPLINUX_ENVIRONMENT_RECIPE", ""),
        "environment recipe",
    )
    content = sha256_value(
        os.environ.get("FPLINUX_ENVIRONMENT_CONTENT", ""),
        "environment content",
    )
    return environment_recipe, content


def root_source(relative: str) -> Path:
    """Resolve a repository-relative source file or directory."""
    return common.ROOT / relative_value(relative, "repository source path")


def target_source(target: str, relative: str) -> Path:
    """Resolve a target-relative source file or directory."""
    return common.ROOT / "targets" / target / relative_value(relative, "target source path")


def selected_profile(target_config: dict[str, Any]) -> str | None:
    """Return the optional profile identity already validated by target loading."""
    profile = target_config.get("profile")
    if profile is None:
        return None
    if not isinstance(profile, str) or not profile:
        fail("target profile is invalid")
    return profile


def bootstrap_implementation_sources(backend: str = "ums9117") -> list[tuple[str, Path]]:
    """Capture the operations that produce the selected bootstrap bytes."""
    sources = (
        _SC9820E_BOOTSTRAP_IMPLEMENTATION_SOURCES
        if backend == "sc9820e"
        else _BOOTSTRAP_IMPLEMENTATION_SOURCES
    )
    return [(relative, require_file(root_source(relative))) for relative in sources]


def kernel_implementation_sources() -> list[tuple[str, Path]]:
    """Capture the preparation and Kbuild operations that produce kernel bytes."""
    return [
        (relative, require_file(root_source(relative)))
        for relative in _KERNEL_IMPLEMENTATION_SOURCES
    ]


def selected_build_sources(target_config: dict[str, Any]) -> tuple[str, ...]:
    """Deliver the current build stages and only the selected storage backends."""
    sources = {
        *_KERNEL_IMPLEMENTATION_SOURCES,
        *_BUILD_STAGE_SOURCES,
        "scripts/fplinux_cli/build/bootstrap/result.py",
    }
    if target_config["bootstrap"].get("backend") == "sc9820e":
        sources.update(_SC9820E_BOOTSTRAP_IMPLEMENTATION_SOURCES)
        sources.add("scripts/fplinux_cli/build/loaders.py")
    else:
        sources.update(_BOOTSTRAP_IMPLEMENTATION_SOURCES)
    if target_config["uboot"]["kind"] == "full":
        sources.add("scripts/fplinux_cli/build/storage/uboot.py")
    if target_config["image"]["kind"] == "ext4-root":
        sources.update(
            (
                "scripts/fplinux_cli/build/storage/ext4.py",
                "scripts/fplinux_cli/build/storage/mke2fs.conf",
                "scripts/fplinux_cli/build/storage/sd.py",
                "scripts/fplinux_cli/build/storage/genimage.cfg.in",
            )
        )
    if target_config["fit"]["kind"] == "sha256":
        sources.add("scripts/fplinux_cli/build/storage/fit.py")
        sources.add("scripts/fplinux_cli/build/storage/linux.its.in")
    return tuple(sorted(sources))

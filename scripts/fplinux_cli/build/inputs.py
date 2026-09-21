# SPDX-License-Identifier: GPL-2.0-only
"""Resolve validated build inputs and output locations."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.values import relative_value

CACHE = Path("/cache")
OUTPUT = Path("/out")


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


def require_sha256(value: object, name: str) -> str:
    """Validate a lowercase SHA-256 digest."""
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return value


def container_image_environment() -> tuple[str, str]:
    """Validate the static image recipe and exact generation supplied by the host."""
    image_recipe = require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE", ""),
        "container image recipe",
    )
    generation = require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_GENERATION", ""),
        "container image generation",
    )
    return image_recipe, generation


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


def implementation_sources() -> list[tuple[str, Path]]:
    """Capture the build package's source files for causal build receipts."""
    directory = require_directory(common.ROOT / "scripts/fplinux_cli/build")
    return [
        (path.relative_to(common.ROOT).as_posix(), require_file(path))
        for path in sorted(directory.glob("*.py"))
    ]

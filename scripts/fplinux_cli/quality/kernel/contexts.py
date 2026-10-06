# SPDX-License-Identifier: GPL-2.0-only
"""Resolve target-owned Linux contexts and their analysis workspaces."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING, Any

from fplinux_cli.build.inputs import CACHE
from fplinux_cli.build.kernel.prepare import prepare_linux
from fplinux_cli.common import ROOT, load_toml
from fplinux_cli.manifests.paths import discover_profiles, discover_targets, normalize_profile
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.build.kernel.state import PreparedLinuxState


def load_sources() -> dict[str, Any]:
    """Load the pinned source lock used by the shared Linux preparer."""
    return load_toml(ROOT / "sources.lock.toml")


def target_profiles(profile: str | None = None) -> tuple[tuple[str, str | None], ...]:
    """Select one global boot policy for every configured board that supports it."""
    profile = normalize_profile(profile)
    return tuple(
        (target, profile)
        for target in discover_targets()
        if (profile or "default") in discover_profiles(target)
    )


def context_label(target: str, profile: str | None) -> str:
    """Return the stable stage label for one target/profile kernel context."""
    return target if profile is None else f"{target}-profile-{profile}"


def sparse_cache_directory(
    target: str, profile: str | None = None, *, build_type: str = "release"
) -> Path:
    """Return the fixed Sparse output directory for one target/profile."""
    root = CACHE / "analysis" / "sparse" / target
    slot = root if profile is None else root / "profiles" / profile
    return slot / "builds" / build_type


def sparse_output(target: str, profile: str | None = None, *, build_type: str = "release") -> Path:
    """Return the one fixed Kbuild output path for one target/profile."""
    return sparse_cache_directory(target, profile, build_type=build_type) / "work"


def reset_sparse_output(
    target: str, profile: str | None = None, *, build_type: str = "release"
) -> Path:
    """Cold-reset the one generated Kbuild output before each analysis."""
    output = sparse_output(target, profile, build_type=build_type)
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True, exist_ok=True)
    return output


def target_context(
    sources: dict[str, Any],
    target: str,
    profile: str | None = None,
    *,
    build_type: str = "release",
) -> tuple[dict[str, Any], dict[str, Any], Path, PreparedLinuxState]:
    """Load one target/profile and prepare the shared Linux integration tree."""
    target_config = load_target(target, profile, build_type=build_type)
    platform = load_platform(target_config["platform"])
    source, prepared_linux = prepare_linux(sources, target, target_config, platform)
    return target_config, platform, source, prepared_linux

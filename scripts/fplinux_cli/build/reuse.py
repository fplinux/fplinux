# SPDX-License-Identifier: GPL-2.0-only
"""Resolve target contexts and exact reusable build bundles."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine import signing
from fplinux_cli.artifacts.bundles import (
    BundleStateError,
    CurrentBundle,
    resolve_current_bundle,
    validate_bundle_files,
)
from fplinux_cli.runtime.bundle_session import bundle_manifest

if TYPE_CHECKING:
    from fplinux_cli.environment.image_state import ImageState
    from fplinux_cli.workspace.capture import WorkspaceSnapshot


@dataclass(frozen=True)
class BuildIdentity:
    """Exact host-visible inputs that authorize bundle reuse."""

    workspace_digest: str
    container_image_recipe: str
    container_image_content: str
    apk_signing_key: str


def profile_log_target(target: str, profile: str | None) -> str:
    """Keep one profile's persistent command logs below its target slot."""
    if profile is None:
        return target
    return f"{target}/profiles/{profile}"


def manifest_matches_identity(manifest: dict[str, Any], identity: BuildIdentity | None) -> bool:
    """Return whether a manifest matches one exact host-visible build identity."""
    return identity is not None and all(
        manifest.get(field) == value
        for field, value in (
            ("workspace_digest", identity.workspace_digest),
            ("container_image_recipe", identity.container_image_recipe),
            ("container_image_content", identity.container_image_content),
            ("apk_signing_key", identity.apk_signing_key),
        )
    )


def required_boot_artifacts(manifest: dict[str, Any]) -> tuple[str, ...]:
    """Return the normalized profile artifacts owned by one build manifest."""
    boot_artifacts = manifest.get("boot_artifacts")
    required = boot_artifacts.get("required") if isinstance(boot_artifacts, dict) else None
    if (
        not isinstance(required, list)
        or not all(isinstance(relative, str) and relative for relative in required)
        or len(required) != len(set(required))
    ):
        message = "build manifest boot artifacts are invalid"
        raise ValueError(message)
    normalized: list[str] = []
    for relative in required:
        path = PurePosixPath(relative)
        if path.is_absolute() or ".." in path.parts or path.as_posix() != relative:
            message = "build manifest boot artifact path is unsafe"
            raise ValueError(message)
        normalized.append(relative)
    return tuple(normalized)


def matching_target_bundle(
    target: str,
    identity: BuildIdentity | None,
    image_relative: str,
    profile: str | None = None,
    *,
    build_type: str = "release",
) -> tuple[CurrentBundle, dict[str, Any]] | None:
    """Return only a fully valid current generation for the exact causal inputs."""
    try:
        bundle = resolve_current_bundle(
            common.ROOT / ".cache/out",
            target,
            profile,
            build_type=build_type,
        )
        manifest = bundle_manifest(bundle)
    except BundleStateError, OSError, UnicodeDecodeError, ValueError:
        return None
    if not manifest_matches_identity(manifest, identity):
        return None
    files = manifest.get("files")
    try:
        required = required_boot_artifacts(manifest)
        if not isinstance(files, dict) or not {image_relative, *required}.issubset(files):
            return None
        validate_bundle_files(bundle.path, files)
    except BundleStateError, OSError, ValueError:
        return None
    return bundle, manifest


def build_identity(
    snapshot: WorkspaceSnapshot, image_state: ImageState | None, cache: Path
) -> BuildIdentity | None:
    """Read the exact host-visible inputs without creating signing state."""
    if image_state is None:
        return None
    try:
        signing_key = signing.signing_key_identity(cache)
    except SystemExit:
        return None
    return BuildIdentity(
        snapshot.recipe,
        image_state.container_image_recipe,
        image_state.image_content,
        signing_key,
    )

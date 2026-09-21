# SPDX-License-Identifier: GPL-2.0-only
"""Resolve target contexts and exact reusable build bundles."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from fplinux_cli import alpine_state, common
from fplinux_cli.bundle_state import BundleStateError, CurrentBundle, resolve_current_bundle
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.manifests.paths import normalize_profile

if TYPE_CHECKING:
    from fplinux_cli.image_state import ImageState
    from fplinux_cli.workspace import WorkspaceSnapshot


MICROSD_BOOT_MODE = "microsd"


MICROSD_BOOT_PROFILE = "microsd-uboot"


PUBLIC_BOOT_MODES = (MICROSD_BOOT_MODE,)


@dataclass(frozen=True)
class BuildIdentity:
    """Exact host-visible inputs that authorize bundle reuse."""

    workspace_digest: str
    container_image_recipe: str
    container_image_generation: str
    apk_signing_key: str


def profile_command(command: str, target: str, profile: str | None) -> str:
    """Render the exact public command for one default or named profile."""
    rendered = f"./fplinux {command} {target}"
    if profile is not None:
        rendered += f" --profile {profile}"
    return rendered


def profile_log_target(target: str, profile: str | None) -> str:
    """Keep one profile's persistent command logs below its target slot."""
    if profile is None:
        return target
    return f"{target}/profiles/{profile}"


def selected_context_profile(
    target: str | None,
    *,
    profile: str | None,
    boot: str | None,
) -> str | None:
    """Resolve one explicit contributor profile or public boot mode without fallback."""
    if profile is not None and boot is not None:
        fail("--boot and --profile cannot be used together")
    if boot is None:
        return normalize_profile(profile)
    if target is None:
        fail(f"boot mode {boot} requires a target")
    if boot == MICROSD_BOOT_MODE:
        return MICROSD_BOOT_PROFILE
    fail(f"boot mode {boot} is not available for target {target}")
    return None


def bundle_manifest(bundle: CurrentBundle) -> dict[str, Any]:
    """Decode manifest bytes already validated by the immutable bundle resolver."""
    manifest = json.loads(bundle.manifest_bytes)
    if not isinstance(manifest, dict):
        message = "build manifest root must be an object"
        raise BundleStateError(message)
    return manifest


def resolve_target_bundle(
    target: str,
    profile: str | None = None,
) -> tuple[CurrentBundle, dict[str, Any]]:
    """Resolve the current bundle pointer exactly once."""
    try:
        bundle = resolve_current_bundle(
            common.ROOT / ".cache/out",
            target,
            profile,
        )
        return bundle, bundle_manifest(bundle)
    except (BundleStateError, OSError, UnicodeDecodeError, ValueError) as error:
        fail(
            "current build is missing or invalid; rebuild it: "
            f"{profile_command('build', target, profile)} ({error})"
        )


def manifest_matches_identity(manifest: dict[str, Any], identity: BuildIdentity | None) -> bool:
    """Return whether a manifest matches one exact host-visible build identity."""
    return identity is not None and all(
        manifest.get(field) == value
        for field, value in (
            ("workspace_digest", identity.workspace_digest),
            ("container_image_recipe", identity.container_image_recipe),
            ("container_image_generation", identity.container_image_generation),
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
) -> tuple[CurrentBundle, dict[str, Any]] | None:
    """Return only a fully valid current generation for the exact causal inputs."""
    try:
        bundle = resolve_current_bundle(
            common.ROOT / ".cache/out",
            target,
            profile,
        )
        manifest = bundle_manifest(bundle)
    except BundleStateError, OSError, UnicodeDecodeError, ValueError:
        return None
    if not manifest_matches_identity(manifest, identity):
        return None
    files = manifest.get("files")
    try:
        required = required_boot_artifacts(manifest)
    except ValueError:
        return None
    if not isinstance(files, dict):
        return None
    for relative in (image_relative, *required):
        record = files.get(relative)
        expected = record.get("sha256") if isinstance(record, dict) else None
        artifact = bundle.path / relative
        if (
            not isinstance(expected, str)
            or artifact.is_symlink()
            or not artifact.is_file()
            or sha256_file(artifact) != expected
        ):
            return None
    return bundle, manifest


def build_identity(
    snapshot: WorkspaceSnapshot, image_state: ImageState | None, cache: Path
) -> BuildIdentity | None:
    """Read the exact host-visible inputs without creating signing state."""
    if image_state is None:
        return None
    try:
        signing_key = alpine_state.signing_key_identity(cache)
    except SystemExit:
        return None
    return BuildIdentity(
        snapshot.recipe,
        image_state.container_image_recipe,
        image_state.image_generation,
        signing_key,
    )

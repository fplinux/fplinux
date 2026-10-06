# SPDX-License-Identifier: GPL-2.0-only
"""Resolve exact build generations and authenticated phone sessions."""

from __future__ import annotations

import importlib.util
import json
import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts.bundles import BundleStateError, resolve_current_bundle
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.manifests.paths import normalize_profile

if TYPE_CHECKING:
    from types import ModuleType

    from fplinux_cli.artifacts.bundles import CurrentBundle

MICROSD_BOOT_MODE = "microsd"
MICROSD_BOOT_PROFILE = "microsd-uboot"
PUBLIC_BOOT_MODES = (MICROSD_BOOT_MODE,)
SSH_HELPER_PATH = "runner/ssh_transport.py"


def profile_command(
    command: str, target: str, profile: str | None, *, build_type: str = "release"
) -> str:
    """Render the exact public command for one default or named profile."""
    rendered = f"./fplinux {command} {target} --build-type {build_type}"
    if profile is not None:
        rendered += f" --profile {profile}"
    return rendered


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
    *,
    build_type: str = "release",
) -> tuple[CurrentBundle, dict[str, Any]]:
    """Resolve the current bundle pointer exactly once."""
    try:
        bundle = resolve_current_bundle(
            common.ROOT / ".cache/out",
            target,
            profile,
            build_type=build_type,
        )
        return bundle, bundle_manifest(bundle)
    except (BundleStateError, OSError, UnicodeDecodeError, ValueError) as error:
        fail(
            "current build is missing or invalid; rebuild it: "
            f"{profile_command('build', target, profile, build_type=build_type)} ({error})"
        )


def _load_bundle_ssh_helper(
    bundle: CurrentBundle,
    manifest: dict[str, Any],
) -> ModuleType:
    """Load only the SSH helper hashed by the selected immutable generation."""
    path = bundle.path / SSH_HELPER_PATH
    files = manifest.get("files")
    record = files.get(SSH_HELPER_PATH) if isinstance(files, dict) else None
    expected = record.get("sha256") if isinstance(record, dict) else None
    if (
        not isinstance(expected, str)
        or path.is_symlink()
        or not path.is_file()
        or sha256_file(path) != expected
    ):
        fail(f"current bundle has no valid SSH transport helper: {path}")
    name = f"fplinux_bundle_ssh_transport_{bundle.generation}"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"current bundle SSH transport helper cannot be loaded: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    required = {
        "load_bundle_context",
        "load_current_session",
        "reacquire_bound_session",
        "run_remote",
        "stream_remote",
        "upload",
        "pull",
        "open_shell",
    }
    if any(not callable(getattr(module, name, None)) for name in required):
        fail("current bundle SSH transport helper has an incompatible API")
    return module


def _current_ssh_session(
    bundle: CurrentBundle,
    manifest: dict[str, Any],
    target: str,
) -> tuple[ModuleType, dict[str, Any]]:
    """Reacquire an authenticated session with the selected device identity."""
    ssh = _load_bundle_ssh_helper(bundle, manifest)
    runtime, identity = ssh.load_bundle_context(bundle.path)
    if runtime.get("target") != target or identity.get("bundle_generation") != bundle.generation:
        fail("current bundle SSH identity disagrees with the selected generation")
    session = ssh.load_current_session(target)
    session = ssh.reacquire_bound_session(session)
    ssh.require_device_identity(session, _manifest_device_identity(manifest))
    return ssh, session


def _manifest_device_identity(manifest: dict[str, Any]) -> str:
    """Return the exact kernel-visible identity declared by one build manifest."""
    device_identity = manifest.get("device_identity")
    if (
        not isinstance(device_identity, str)
        or len(device_identity) != 64
        or any(character not in "0123456789abcdef" for character in device_identity)
    ):
        fail("current bundle device identity is invalid")
    return device_identity


def current_target_ssh_session(
    target: str, *, profile: str | None = None, build_type: str = "release"
) -> tuple[ModuleType, dict[str, Any]]:
    """Resolve the authenticated session for one exact target and build profile."""
    bundle, manifest = resolve_target_bundle(
        target, normalize_profile(profile), build_type=build_type
    )
    return _current_ssh_session(bundle, manifest, target)

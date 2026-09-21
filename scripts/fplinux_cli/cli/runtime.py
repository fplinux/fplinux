# SPDX-License-Identifier: GPL-2.0-only
"""Run and inspect a selected phone session."""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli import image_state as image_states
from fplinux_cli import workspace as workspaces
from fplinux_cli.cli import bundles as bundles_commands
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.environment import images
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.paths import normalize_profile

if TYPE_CHECKING:
    from pathlib import Path
    from types import ModuleType

    from fplinux_cli.bundle_state import CurrentBundle


SSH_HELPER_PATH = "runner/ssh_transport.py"


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


def _keyboard_client(bundle: CurrentBundle) -> Path:
    client = bundle.path / "host/fplinux-usb-keyboard"
    if client.is_symlink() or not client.is_file():
        fail(f"current bundle has no valid USB keyboard client: {client}")
    return client


def _keyboard_connection(config: dict[str, Any]) -> list[str]:
    gadget = config["runtime"]["usb"]["linux_gadget"]
    return [
        "--vid",
        f"{gadget['vendor_id']:04x}",
        "--pid",
        f"{gadget['product_id']:04x}",
        "--wait",
        str(gadget["wait_seconds"]),
    ]


def _keyboard_interface(config: dict[str, Any]) -> str:
    """Return the runtime-declared generic-serial keyboard interface."""
    return str(config["runtime"]["usb"]["linux_gadget"]["keyboard_interface"])


def console_target(  # noqa: PLR0913 -- public CLI modes remain explicit.
    target: str,
    *,
    profile: str | None = None,
    keyboard: str | None,
    exec_command: str | None,
    upload: list[str] | None,
    pull: list[str] | None,
) -> None:
    """Open the SSH session, or forward one evdev keyboard over USB."""
    config = targets.load_target(target, profile)
    bundle, manifest = bundles_commands.resolve_target_bundle(target, profile)
    if keyboard is None:
        ssh_transport, session = _current_ssh_session(bundle, manifest, target)
        if exec_command is not None:
            result = ssh_transport.run_remote(session, exec_command)
            if result.returncode:
                raise SystemExit(result.returncode)
            return
        if upload is not None:
            ssh_transport.upload(session, upload[0], upload[1])
            return
        if pull is not None:
            ssh_transport.pull(session, pull[0], pull[1])
            return
        ssh_transport.open_shell(session)
        return
    client = _keyboard_client(bundle)
    arguments = [
        str(client),
        *_keyboard_connection(config),
        "--interface",
        _keyboard_interface(config),
        "--keyboard",
        keyboard,
    ]
    os.execv(client, arguments)


def verify_booted(target: str, *, profile: str | None = None) -> None:
    """Compare the running kernel identity with the current bundle."""
    profile = normalize_profile(profile)
    bundle, manifest = bundles_commands.resolve_target_bundle(target, profile)
    snapshot = workspaces.target_workspace_snapshot(target, profile)
    image_recipe = images.container_image_recipe_digest()
    image_state = image_states.load_image_state(common.ROOT / ".cache", image_recipe)
    identity = bundles_commands.build_identity(snapshot, image_state, common.ROOT / ".cache")
    if not bundles_commands.manifest_matches_identity(manifest, identity):
        fail(
            "build output is stale; rebuild it: "
            f"{bundles_commands.profile_command('build', target, profile)}"
        )
    device_identity = _manifest_device_identity(manifest)
    _current_ssh_session(bundle, manifest, target)
    print(f"verify: the phone runs the current build ({device_identity[:16]})")


def current_target_ssh_session(
    target: str, *, profile: str | None = None
) -> tuple[ModuleType, dict[str, Any]]:
    """Resolve the authenticated session for one exact target and build profile."""
    bundle, manifest = bundles_commands.resolve_target_bundle(target, normalize_profile(profile))
    return _current_ssh_session(bundle, manifest, target)


def _runnable_target_runner(
    target: str,
    *,
    profile: str | None = None,
    boot: str | None = None,
) -> Path:
    """Resolve the fixed shared runner for one runnable bundle."""
    selected_profile = bundles_commands.selected_context_profile(
        target, profile=profile, boot=boot
    )
    if selected_profile is not None:
        target_config = targets.load_target(target, selected_profile)
        if not target_config["runtime"]["runnable"]:
            fail(f"profile is build-only and cannot be run: {target}/{selected_profile}")
    bundle, manifest = bundles_commands.resolve_target_bundle(target, selected_profile)
    if selected_profile is not None:
        boot_artifacts = manifest.get("boot_artifacts")
        if not isinstance(boot_artifacts, dict) or boot_artifacts.get("runnable") is not True:
            fail(f"profile bundle is build-only and cannot be run: {target}/{selected_profile}")
    runner = bundle.path / "runner/run.py"
    if runner.is_symlink() or not runner.is_file():
        fail(f"current bundle has no valid runner: {runner}")
    return runner


def run_target(
    target: str,
    *,
    profile: str | None = None,
    boot: str | None = None,
) -> None:
    """Run the fixed shared runner from a successful target bundle."""
    runner = _runnable_target_runner(target, profile=profile, boot=boot)
    os.execv(os.fsencode(runner), [os.fsencode(runner)])


def run_target_noninteractive(target: str, *, profile: str | None = None) -> None:
    """Run a loader to its authenticated handoff without taking over this CLI process."""
    runner = _runnable_target_runner(target, profile=profile)
    result = subprocess.run(
        [os.fsencode(runner)],
        stdin=subprocess.DEVNULL,
        check=False,
    )
    if result.returncode:
        fail(f"RAM loader failed with exit status {result.returncode}")

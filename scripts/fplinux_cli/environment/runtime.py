# SPDX-License-Identifier: GPL-2.0-only
"""Validate and publish the single prepared build-environment rootfs."""

from __future__ import annotations

import re
import subprocess
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail, read_json_object
from fplinux_cli.data_paths import data_root

from .definition import environment_recipe_digest, load_environment_lock
from .kern import KERN_PROBE_TIMEOUT, kern_available, kern_box_name, kern_environment, require_kern
from .state import (
    EnvironmentState,
    EnvironmentStateError,
    environment_state_path,
    load_environment_state,
    publish_environment_state,
)

if TYPE_CHECKING:
    from pathlib import Path


def environment_arguments(rootfs: Path) -> list[str]:
    """Supply the rootfs and the shared tool environment for every Kern invocation."""
    arguments = ["--rootfs", str(rootfs)]
    for setting in (
        "LANG=C.UTF-8",
        "LC_ALL=C.UTF-8",
        "TZ=UTC",
        "PYTHONDONTWRITEBYTECODE=1",
        "PYTEST_DISABLE_PLUGIN_AUTOLOAD=1",
        "NPM_CONFIG_AUDIT=false",
        "NPM_CONFIG_FUND=false",
        "NPM_CONFIG_UPDATE_NOTIFIER=false",
        (
            "PATH=/opt/quality/bin:/opt/quality/node/bin:"
            "/opt/quality/node-tools/node_modules/.bin:/usr/local/sbin:/usr/local/bin:"
            "/usr/sbin:/usr/bin:/sbin:/bin"
        ),
    ):
        arguments.extend(("--env", setting))
    return arguments


def environment_rootfs(cache_root: Path, state: EnvironmentState) -> Path:
    """Resolve the exact rootfs generation named by a validated receipt."""
    return cache_root / "environment" / f"rootfs-{state.generation}"


def probe_environment_state(
    kern: str, rootfs: Path, recipe: str, *, cache: Path | None = None
) -> EnvironmentState | None:
    """Measure logical ownership and installed content through a ranged read-only box."""
    if rootfs.is_symlink() or not rootfs.is_dir():
        return None
    collector = "/tmp/fplinux-environment-content.py"  # noqa: S108 -- private read-only box mount.
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("environment-probe"),
                *environment_arguments(rootfs),
                "--uid-range",
                "--read-only",
                "--network",
                "none",
                "--quiet",
                "--volume",
                f"{ROOT / 'scripts/fplinux_cli/environment/image_content.py'}:{collector}:ro",
                "--",
                "sh",
                "-ceu",
                (f"cat /etc/fplinux-environment-state; python3 -B {collector}"),
            ],
            cwd=ROOT,
            env=kern_environment(cache=cache),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern environment lookup timed out after {KERN_PROBE_TIMEOUT}s")
    lines = result.stdout.splitlines()
    if result.returncode or len(lines) != 4:
        return None
    declared_recipe, generation, content, measured_content = lines
    if declared_recipe != recipe or measured_content != content:
        return None
    try:
        return EnvironmentState(declared_recipe, generation, content)
    except EnvironmentStateError:
        return None


def current_environment_state(
    kern: str, recipe: str, *, cache: Path | None = None
) -> EnvironmentState | None:
    """Require both the current receipt and a fresh exact installed-content match."""
    cache_root = data_root(ROOT) if cache is None else cache
    state = load_environment_state(cache_root, recipe)
    if state is None:
        return None
    observed = probe_environment_state(
        kern, environment_rootfs(cache_root, state), recipe, cache=cache_root
    )
    return state if observed == state else None


def remove_environment_rootfs(path: Path, *, cache: Path | None = None) -> None:
    """Remove one known candidate or superseded rootfs using its logical owner mapping."""
    selected_root = data_root(ROOT) if cache is None else cache
    expected_parent = selected_root / "environment"
    if (
        path.parent != expected_parent
        or path.is_symlink()
        or re.fullmatch(r"(?:candidate-[a-zA-Z0-9_-]+|rootfs-[0-9a-f]{64})", path.name) is None
    ):
        fail(f"refusing to remove an unrecognized environment directory: {path}")
    if not path.exists():
        return
    try:
        result = subprocess.run(
            ["unshare", "--map-auto", "--map-root-user", "--", "rm", "-rf", "--", str(path)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"environment rootfs removal timed out after {KERN_PROBE_TIMEOUT}s: {path}")
    if result.returncode:
        fail(result.stderr.strip() or f"environment rootfs removal failed: {path}")


def _previous_state(cache_root: Path) -> EnvironmentState | None:
    payload = read_json_object(environment_state_path(cache_root))
    if payload is None:
        return None
    recipe = payload.get("recipe")
    return load_environment_state(cache_root, recipe) if isinstance(recipe, str) else None


def publish_environment(
    kern: str, candidate: Path, state: EnvironmentState, *, cache: Path | None = None
) -> EnvironmentState:
    """Publish a verified candidate before discarding the previous known working tree."""
    cache_root = data_root(ROOT) if cache is None else cache
    directory = cache_root / "environment"
    if candidate.parent != directory or not candidate.name.startswith("candidate-"):
        fail(f"environment candidate is outside its preparation directory: {candidate}")
    if probe_environment_state(kern, candidate, state.recipe, cache=cache_root) != state:
        fail("prepared environment failed its exact installed-content check")
    previous = _previous_state(cache_root)
    destination = environment_rootfs(cache_root, state)
    if destination.exists() or destination.is_symlink():
        if probe_environment_state(kern, destination, state.recipe, cache=cache_root) != state:
            fail("environment generation already exists with different installed content")
        publish_environment_state(cache_root, state)
        remove_environment_rootfs(candidate, cache=cache_root)
    else:
        candidate.rename(destination)
        try:
            publish_environment_state(cache_root, state)
        except BaseException:
            destination.rename(candidate)
            raise
    if previous is not None and previous.generation != state.generation:
        remove_environment_rootfs(environment_rootfs(cache_root, previous), cache=cache_root)
    return state


def require_environment(
    *, target: str | None = None, cache: Path | None = None
) -> tuple[str, Path, EnvironmentState]:
    """Require the exact environment previously prepared by an explicit setup command."""
    lock = load_environment_lock()
    selected_root = data_root(ROOT) if cache is None else cache
    recipe = environment_recipe_digest(lock)
    if kern_available(lock, cache=selected_root):
        kern = require_kern(lock, cache=selected_root)
        current = current_environment_state(kern, recipe, cache=selected_root)
        if current is not None:
            return kern, environment_rootfs(selected_root, current), current
    selector = "--all" if target is None else target
    return fail(
        f"the current pinned environment is missing or stale; run ./fplinux setup {selector}"
    )

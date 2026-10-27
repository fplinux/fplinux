# SPDX-License-Identifier: GPL-2.0-only
"""Preserve and restore the prepared rootfs attached to a dependency snapshot."""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.data_paths import data_root
from fplinux_cli.environment.definition import environment_recipe_digest, load_environment_lock
from fplinux_cli.environment.kern import (
    ensure_project_directory,
    install_kern,
    kern_environment,
    require_kern,
)
from fplinux_cli.environment.runtime import (
    current_environment_state,
    environment_rootfs,
    publish_environment,
    remove_environment_rootfs,
)
from fplinux_cli.environment.state import EnvironmentState

from .rootfs_archive import require_rootfs_archive
from .snapshots import copy_verified, object_path, read_snapshot, require_matching_environment

if TYPE_CHECKING:
    from fplinux_cli.reporting.run import RunReporter


_ENVIRONMENT_TIMEOUT = 2 * 60 * 60
_MAPPED_ROOT = ["unshare", "--map-auto", "--map-root-user", "--"]


def save_environment(
    directory: Path, reporter: RunReporter, *, cache: Path | None = None
) -> dict[str, Any]:
    """Preserve logical owners, modes, links and installed bytes in one native archive."""
    lock = load_environment_lock()
    cache = data_root(common.ROOT) if cache is None else cache
    kern = require_kern(lock, cache=cache)
    recipe = environment_recipe_digest(lock)
    state = current_environment_state(kern, recipe, cache=cache)
    if state is None:
        fail("current build environment is unavailable; run ./fplinux setup first")
    manifest_path = directory / "manifest.json"
    if manifest_path.exists() or manifest_path.is_symlink():
        saved: dict[str, Any] | None = read_snapshot(directory)["environment"]
        if saved is not None:
            require_matching_environment(EnvironmentState(**saved["state"]), state)
            return saved
    rootfs = environment_rootfs(cache, state)
    with tempfile.TemporaryDirectory(dir=directory, prefix=".environment-") as temporary:
        archive = Path(temporary) / "environment.tar.gz"
        with reporter.stage("environment-save") as stage:
            stage.run(
                [
                    *_MAPPED_ROOT,
                    "tar",
                    "--create",
                    "--gzip",
                    "--numeric-owner",
                    "--file",
                    str(archive),
                    "--directory",
                    str(rootfs),
                    ".",
                ],
                cwd=common.ROOT,
                env=kern_environment(cache=cache),
                timeout=_ENVIRONMENT_TIMEOUT,
            )
        require_rootfs_archive(archive)
        if current_environment_state(kern, recipe, cache=cache) != state:
            fail("source build environment changed during snapshot export")
        digest = sha256_file(archive)
        copy_verified(archive, object_path(directory, digest), digest)
        return {"sha256": digest, "bytes": archive.stat().st_size, "state": state.payload()}


def restore_environment(
    directory: Path,
    environment: dict[str, Any],
    reporter: RunReporter,
    *,
    cache: Path | None = None,
) -> None:
    """Publish a restored rootfs only after archive and installed-content validation."""
    lock = load_environment_lock()
    cache = data_root(common.ROOT) if cache is None else cache
    recipe = environment_recipe_digest(lock)
    state = EnvironmentState(**environment["state"])
    if state.recipe != recipe:
        fail("saved build environment does not match this checkout's exact environment recipe")
    archive = object_path(directory, environment["sha256"])
    require_rootfs_archive(archive)
    kern = install_kern(lock, offline=True, cache=cache)
    parent = ensure_project_directory(cache / "environment")
    candidate = Path(tempfile.mkdtemp(dir=parent, prefix="candidate-"))
    try:
        with reporter.stage("environment-load") as stage:
            stage.run(
                [
                    *_MAPPED_ROOT,
                    "tar",
                    "--extract",
                    "--gzip",
                    "--same-owner",
                    "--same-permissions",
                    "--numeric-owner",
                    "--file",
                    str(archive),
                    "--directory",
                    str(candidate),
                ],
                cwd=common.ROOT,
                env=kern_environment(cache=cache),
                timeout=_ENVIRONMENT_TIMEOUT,
            )
        publish_environment(kern, candidate, state, cache=cache)
    finally:
        if candidate.exists():
            remove_environment_rootfs(candidate, cache=cache)

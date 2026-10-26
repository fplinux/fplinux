# SPDX-License-Identifier: GPL-2.0-only
"""Prepare one input set for a release and restore it in each build job."""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.artifacts.release import capture_public_sources
from fplinux_cli.common import fail, is_sha256
from fplinux_cli.dependencies.distribution import read_dependency_distribution
from fplinux_cli.dependencies.workflow import (
    create_dependencies,
    export_dependencies,
    import_dependencies,
    restore_dependencies,
)


def prepare_inputs(output: Path) -> None:
    """Preserve originals once before the independent jobs install their environments."""
    output = output.absolute()
    if output.exists() or output.is_symlink():
        fail(f"release input output must be a new directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".release-inputs-", dir=output.parent) as temporary:
        snapshot = Path(temporary) / "snapshot"
        create_dependencies(snapshot, offline=False, sources=(), inputs_only=True)
        export_dependencies(snapshot, output)
    distribution = read_dependency_distribution(output, verify_parts=False)
    workflow_output = os.environ.get("GITHUB_OUTPUT")
    if workflow_output:
        with Path(workflow_output).open("a", encoding="utf-8") as stream:
            stream.write(f"snapshot={distribution.snapshot}\n")
            stream.write(f"release_id={distribution.release.release_id}\n")
    print(f"Release inputs: {distribution.release.release_id}: {distribution.snapshot}")


def restore_inputs(directory: Path, *, snapshot: str) -> None:
    """Restore only the input artifact selected by this exact workflow run."""
    if not is_sha256(snapshot):
        fail("release input snapshot must be a SHA-256 digest")
    directory = directory.absolute()
    distribution = read_dependency_distribution(directory, verify_parts=False)
    if distribution.snapshot != snapshot:
        fail("release input artifact differs from the prepared snapshot")
    if distribution.release != capture_public_sources().release:
        fail("release input artifact belongs to another source release")
    cache = common.ROOT / ".cache"
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".release-inputs-", dir=cache) as temporary:
        restored = Path(temporary) / "snapshot"
        import_dependencies(directory, restored)
        restore_dependencies(restored, inputs_only=True)

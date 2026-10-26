# SPDX-License-Identifier: GPL-2.0-only
"""Explicitly install fresh upstream inputs before authorizing a local main push."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tarfile
from datetime import UTC, datetime
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.artifacts.release import (
    SOURCE_IDENTITY_NAME,
    ReleaseContext,
    capture_public_sources,
)
from fplinux_cli.common import canonical_json_bytes, fail, is_sha256, replace_file_atomically
from fplinux_cli.dependencies.snapshots import read_snapshot
from fplinux_cli.environment.state import EnvironmentState, environment_state_path

if TYPE_CHECKING:
    from pathlib import Path

# Fresh originals, their preserved copy and the installed rootfs need separate space.
_MINIMUM_FREE_BYTES = 10 * 1024**3


def preflight_receipt(root: Path, commit: str) -> Path:
    """Locate the completed preparation for an exact incoming source commit."""
    if re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit) is None:
        fail("preflight requires a complete lowercase Git commit SHA")
    return root / ".cache/preflight" / f"{commit}.json"


def require_preflight(root: Path, commit: str) -> dict[str, Any]:
    """Read a successful preparation without executing or downloading incoming code."""
    receipt = common.read_json_object(preflight_receipt(root, commit))
    if receipt is None or receipt.get("source_commit") != commit:
        fail(
            f"main commit {commit} has no completed clean preparation; run ci preflight explicitly"
        )
    for field in ("source_snapshot", "snapshot", "environment_recipe", "environment_content"):
        if not is_sha256(receipt.get(field)):
            fail(f"main commit {commit} has an incomplete preparation: {field}")
    if not isinstance(receipt.get("completed_at"), str):
        fail(f"main commit {commit} has no preparation completion time")
    return receipt


def _run_preflight_command(checkout: Path, *arguments: str) -> None:
    result = subprocess.run([str(checkout / "fplinux"), *arguments], cwd=checkout, check=False)
    if result.returncode:
        fail(f"clean preparation stopped: {' '.join(arguments)} (exit {result.returncode})")


def prepare_push(commit: str, directory: Path) -> None:
    """Fetch from upstream into a new checkout, install, and retain the exact results."""
    receipt = preflight_receipt(common.ROOT, commit)
    directory = directory.absolute()
    if directory.exists() or directory.is_symlink():
        fail(f"clean preparation requires a new directory: {directory}")
    if directory.resolve().is_relative_to((common.ROOT / ".cache").resolve()):
        fail("clean preparation must use a separate directory outside the working cache")
    directory.parent.mkdir(parents=True, exist_ok=True)
    available = shutil.disk_usage(directory.parent).free
    if available < _MINIMUM_FREE_BYTES:
        fail(
            "clean preparation needs at least 10 GiB free; "
            f"available: {available / 1024**3:.1f} GiB"
        )
    print(f"Clean preparation for {commit}; available: {available / 1024**3:.1f} GiB", flush=True)
    directory.mkdir(mode=0o700)
    receipt.unlink(missing_ok=True)
    archive_path = directory / "checkout.tar"
    with archive_path.open("wb") as archive_file:
        result = subprocess.run(
            ["git", "archive", "--format=tar", commit],
            cwd=common.ROOT,
            stdout=archive_file,
            check=False,
            timeout=120,
        )
    if result.returncode:
        fail(f"cannot read the exact preparation commit: {commit}")
    checkout = directory / "checkout"
    checkout.mkdir()
    with tarfile.open(archive_path) as archive:
        archive.extractall(checkout, filter="data")
    archive_path.unlink()
    captured = capture_public_sources(checkout)
    release = ReleaseContext(commit[:12], commit, captured.release.source_snapshot)
    (checkout / SOURCE_IDENTITY_NAME).write_bytes(canonical_json_bytes(release.fields()))
    originals = directory / "originals"
    _run_preflight_command(checkout, "dependencies", "create", str(originals), "--inputs-only")
    _run_preflight_command(checkout, "setup", "--offline")
    _run_preflight_command(checkout, "doctor")
    _run_preflight_command(checkout, "dependencies", "verify", str(originals))
    manifest = read_snapshot(originals)
    environment_path = environment_state_path(checkout / ".cache")
    try:
        state = EnvironmentState(**json.loads(environment_path.read_bytes()))
    except (OSError, ValueError, TypeError) as error:
        fail(f"clean preparation did not retain a valid environment: {error}")
    completed = {
        **release.fields(),
        "snapshot": manifest["snapshot"],
        "environment_recipe": state.recipe,
        "environment_content": state.content,
        "completed_at": datetime.now(UTC).isoformat(),
        "directory": str(directory),
    }
    receipt.parent.mkdir(parents=True, exist_ok=True)
    replace_file_atomically(receipt, canonical_json_bytes(completed), 0o600)
    print(f"Clean preparation completed: {receipt}")

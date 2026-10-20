# SPDX-License-Identifier: GPL-2.0-only
"""Exercise rootfs snapshot archives through real mapped filesystem processes."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.dependencies import environment_archive
from fplinux_cli.dependencies.snapshots import preserve_inputs, publish_snapshot, read_snapshot
from fplinux_cli.environment import runtime
from fplinux_cli.environment.state import EnvironmentState, load_environment_state
from fplinux_cli.reporting.run import RunReporter

if TYPE_CHECKING:
    from collections.abc import Iterator

_ROOTFS_PEER = (
    Path(__file__).resolve().parents[2] / "fixtures/dependencies/environment-export/rootfs_peer.py"
)
_EXPECTED = {
    "tool": [1234, 2345, 0o6755],
    "private": [1000, 1000, 0o700],
    "tool_bytes": "installed executable\n",
    "private_bytes": "builder data\n",
    "symlink": "/usr/bin/tool",
    "hard_link": True,
}


@pytest.fixture
def namespace() -> list[str]:
    """Require the same numeric ownership mapping as prepared rootfs publication."""
    unshare = shutil.which("unshare")
    if unshare is None:
        pytest.fail("rootfs archive test requires unshare")
    command = [unshare, "--map-auto", "--map-root-user", "--"]
    result = subprocess.run(
        [*command, "true"], capture_output=True, text=True, check=False, timeout=30
    )
    if result.returncode:
        pytest.fail("subordinate namespace is unavailable: " + result.stderr.strip())
    return command


@pytest.fixture
def snapshot_root(namespace: list[str]) -> Iterator[Path]:
    """Remove only this scenario's mapped-user files after success or failure."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        try:
            yield root
        finally:
            subprocess.run(
                [*namespace, "rm", "--recursive", "--force", "--", str(root)],
                check=True,
                timeout=30,
            )


def _inspect(namespace: list[str], rootfs: Path) -> dict[str, object]:
    result = subprocess.run(
        [*namespace, sys.executable, str(_ROOTFS_PEER), "inspect", str(rootfs)],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert result.returncode == 0, result.stderr
    value: dict[str, object] = json.loads(result.stdout)
    return value


@pytest.mark.parametrize("content_matches", [True, False], ids=("round-trip", "content-rejected"))
def test_environment_archive_retains_owners_links_and_modes_before_publication(
    snapshot_root: Path, namespace: list[str], *, content_matches: bool
) -> None:
    """The real archive consumer preserves metadata and rejects a failed content probe."""
    root = snapshot_root
    source_checkout = root / "source"
    source = source_checkout / ".cache/environment" / ("rootfs-" + "b" * 64)
    source.mkdir(parents=True)
    prepared = subprocess.run(
        [*namespace, sys.executable, str(_ROOTFS_PEER), "prepare", str(source)],
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    assert prepared.returncode == 0, prepared.stderr
    assert (source / "usr/bin/tool").stat().st_uid != os.geteuid()
    state = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    directory = root / "snapshot"
    manifest = preserve_inputs([], {}, directory, cache=source_checkout / ".cache", offline=True)
    reporter = RunReporter("dependencies", root / "logs", "logs", verbose=False)
    try:
        with (
            mock.patch.object(common, "ROOT", source_checkout),
            mock.patch.object(environment_archive, "require_kern", return_value="kern"),
            mock.patch.object(environment_archive, "load_environment_lock", return_value={}),
            mock.patch.object(
                environment_archive, "environment_recipe_digest", return_value=state.recipe
            ),
            mock.patch.object(
                environment_archive, "current_environment_state", return_value=state
            ),
        ):
            manifest["environment"] = environment_archive.save_environment(directory, reporter)
        publish_snapshot(directory, manifest)
        environment = read_snapshot(directory)["environment"]
        assert environment["state"] == state.payload()
        archive = directory / "objects/sha256" / environment["sha256"]
        with tarfile.open(archive, "r:gz") as bundle:
            tool = bundle.getmember("./usr/bin/tool")
            assert (tool.uid, tool.gid, tool.mode) == (1234, 2345, 0o6755)
            assert bundle.getmember("./tool").linkname == "/usr/bin/tool"

        restored_checkout = root / "restored"
        restored_checkout.mkdir()

        def probe(_kern: str, candidate: Path, _recipe: str) -> EnvironmentState | None:
            assert _inspect(namespace, candidate) == _EXPECTED
            return state if content_matches else None

        with (
            mock.patch.object(common, "ROOT", restored_checkout),
            mock.patch.object(runtime, "ROOT", restored_checkout),
            mock.patch.object(environment_archive, "install_kern", return_value="kern"),
            mock.patch.object(environment_archive, "load_environment_lock", return_value={}),
            mock.patch.object(
                environment_archive, "environment_recipe_digest", return_value=state.recipe
            ),
            mock.patch.object(runtime, "probe_environment_state", side_effect=probe),
        ):
            if content_matches:
                environment_archive.restore_environment(directory, environment, reporter)
            else:
                with pytest.raises(SystemExit, match="installed-content check"):
                    environment_archive.restore_environment(directory, environment, reporter)
        restored_cache = restored_checkout / ".cache"
        candidates = list((restored_cache / "environment").glob("candidate-*"))
        assert candidates == []
        if content_matches:
            assert load_environment_state(restored_cache, state.recipe) == state
            restored = runtime.environment_rootfs(restored_cache, state)
            assert _inspect(namespace, restored) == _EXPECTED
        else:
            assert load_environment_state(restored_cache, state.recipe) is None
            assert not runtime.environment_rootfs(restored_cache, state).exists()
    finally:
        reporter.finish()

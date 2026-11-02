# SPDX-License-Identifier: GPL-2.0-only
"""Verify selected staged source imports in an isolated process."""

from __future__ import annotations

import subprocess
import sys
from typing import TYPE_CHECKING

import fplinux_cli.workspace.build_inputs as workspace_inputs
import fplinux_cli.workspace.capture as workspace_capture
import fplinux_cli.workspace.staging as workspace_staging
import pytest

from tests import ROOT
from tests.small.workspace.workspace_fixtures import workspace_root

if TYPE_CHECKING:
    from pathlib import Path


class WorkspaceSnapshotImportTests:
    """Exercise staged source imports without the checkout."""

    @pytest.mark.parametrize("profile", [None, "microsd-uboot"], ids=("ram", "microsd"))
    def test_staged_source_fetch_imports_and_reuses_verified_bytes_without_checkout(
        self, profile: str | None, tmp_path: Path
    ) -> None:
        """The selected build closure imports and checks cached sources in an isolated process."""
        snapshot = workspace_capture.workspace_snapshot(
            workspace_inputs.target_build_source_files("nokia-ta1618", profile)
        )
        with workspace_root(tmp_path):
            staged = workspace_staging.stage_workspace_snapshot(snapshot)
        cache = tmp_path / "downloads"
        cache.mkdir()
        (cache / "source.tar").write_bytes(b"abc")
        fixture = ROOT / "tests/host_process/workspace/fixtures/snapshot/staged_source_fetch.py"
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(fixture), str(staged), str(cache)],
            cwd=tmp_path,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        assert result.returncode == 0, result.stdout + result.stderr

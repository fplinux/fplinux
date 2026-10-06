# SPDX-License-Identifier: GPL-2.0-only
"""Snapshot materialization, exact reuse and bounded workspace disposal."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.workspace.capture as workspace_capture
import fplinux_cli.workspace.staging as workspace_staging
import pytest

from tests.small.workspace.workspace_fixtures import WorkspaceSourceFixture

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


class WorkspaceMaterializationTests(WorkspaceSourceFixture):
    """Keep cache materialization causally bound to an already read snapshot."""

    def test_materialization_uses_precomputed_bytes_after_checkout_changes(
        self, tmp_path: Path
    ) -> None:
        """Staging reads only the snapshot, so a later checkout edit cannot leak in."""
        root = tmp_path
        source = self._source(root, contents=b"snapshot", mode=0o751)
        snapshot = workspace_capture.workspace_snapshot([("bin/tool", source)])
        source.write_bytes(b"new-checkout")
        source.chmod(0o644)

        with mock.patch.object(workspace_staging, "ROOT", root):
            staged = workspace_staging.stage_workspace_snapshot(snapshot)

        output = staged / "bin/tool"
        assert (output.read_bytes()) == (b"snapshot")
        assert (output.stat().st_mode & 0o777) == (0o751)

    def test_valid_exact_cache_is_a_hit_without_new_staging_directory(
        self, tmp_path: Path
    ) -> None:
        """An exact cache entry returns before tempfile creation."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        with mock.patch.object(workspace_staging, "ROOT", root):
            expected = workspace_staging.stage_workspace_snapshot(snapshot)
            with mock.patch(
                "fplinux_cli.workspace.staging.tempfile.mkdtemp",
                side_effect=AssertionError("cache hit must not stage"),
            ):
                actual = workspace_staging.stage_workspace_snapshot(snapshot)

        assert (actual) == (expected)

    def test_discard_removes_only_a_completed_exact_target_workspace(self, tmp_path: Path) -> None:
        """A target workspace is disposable immediately after its snapshot consumer exits."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        with mock.patch.object(workspace_staging, "ROOT", root):
            workspace = workspace_staging.stage_workspace_snapshot(snapshot)
            workspace_staging.discard_staged_workspace_snapshot(snapshot, workspace)

        assert not (workspace.exists())

    def test_discard_removes_only_a_completed_exact_quality_workspace(
        self, tmp_path: Path
    ) -> None:
        """A completed quality workspace has the same bounded retention contract."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        with mock.patch.object(workspace_staging, "ROOT", root):
            workspace = workspace_staging.stage_quality_workspace_snapshot(snapshot)
            workspace_staging.discard_staged_quality_workspace_snapshot(snapshot, workspace)

        assert not (workspace.exists())

    def test_discard_refuses_a_path_outside_its_snapshot_slot(self, tmp_path: Path) -> None:
        """The discard API cannot be redirected to another cache directory."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        wrong = root / ".cache/workspaces/not-the-snapshot"
        wrong.mkdir(parents=True)
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            pytest.raises(SystemExit, match="outside its managed cache slot"),
        ):
            workspace_staging.discard_staged_workspace_snapshot(snapshot, wrong)

        assert wrong.is_dir()

    def test_discard_refuses_a_symlinked_workspace_or_mismatched_marker(
        self, tmp_path: Path
    ) -> None:
        """A stale link or altered marker is not recognised as disposable cache state."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        expected = root / ".cache/workspaces" / snapshot.recipe
        external = root / "external"
        external.mkdir()
        expected.parent.mkdir(parents=True)
        expected.symlink_to(external, target_is_directory=True)
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            pytest.raises(SystemExit, match="path is missing or invalid"),
        ):
            workspace_staging.discard_staged_workspace_snapshot(snapshot, expected)

        expected.unlink()
        with mock.patch.object(workspace_staging, "ROOT", root):
            workspace = workspace_staging.stage_workspace_snapshot(snapshot)
            (workspace / ".fplinux-workspace").write_text("wrong\n", encoding="utf-8")
            with pytest.raises(SystemExit, match="marker is missing or invalid"):
                workspace_staging.discard_staged_workspace_snapshot(snapshot, workspace)

        assert workspace.is_dir()

    @pytest.mark.parametrize(
        ("namespace_name", "stage", "discard"),
        [
            pytest.param(
                "workspaces",
                workspace_staging.stage_workspace_snapshot,
                workspace_staging.discard_staged_workspace_snapshot,
                id="target-workspace",
            ),
            pytest.param(
                "quality-workspaces",
                workspace_staging.stage_quality_workspace_snapshot,
                workspace_staging.discard_staged_quality_workspace_snapshot,
                id="quality-workspace",
            ),
        ],
    )
    def test_stage_and_discard_refuse_symlinked_cache_namespaces(
        self,
        tmp_path: Path,
        namespace_name: str,
        stage: Callable[[workspace_capture.WorkspaceSnapshot], Path],
        discard: Callable[[workspace_capture.WorkspaceSnapshot, Path], None],
    ) -> None:
        """Managed cache components never redirect workspace writes or removals externally."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        external = root / f"external-{namespace_name}"
        sentinel = external / "sentinel"
        external.mkdir()
        sentinel.write_text("preserve\n", encoding="utf-8")
        namespace = root / ".cache" / namespace_name
        namespace.parent.mkdir(exist_ok=True)
        namespace.symlink_to(external, target_is_directory=True)
        expected = namespace / snapshot.recipe
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            pytest.raises(SystemExit, match="workspace namespace is missing or invalid"),
        ):
            stage(snapshot)
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            pytest.raises(SystemExit, match="workspace namespace is missing or invalid"),
        ):
            discard(snapshot, expected)

        assert (sentinel.read_text(encoding="utf-8")) == ("preserve\n")

    def test_stage_and_discard_refuse_a_symlinked_cache_root(self, tmp_path: Path) -> None:
        """The namespace check also refuses a redirected `.cache` component."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        external = root / "external-cache"
        sentinel = external / "sentinel"
        external.mkdir()
        sentinel.write_text("preserve\n", encoding="utf-8")
        cache = root / ".cache"
        cache.symlink_to(external, target_is_directory=True)
        expected = cache / "workspaces" / snapshot.recipe
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            pytest.raises(SystemExit, match="workspace cache root is missing or invalid"),
        ):
            workspace_staging.stage_workspace_snapshot(snapshot)
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            pytest.raises(SystemExit, match="workspace cache root is missing or invalid"),
        ):
            workspace_staging.discard_staged_workspace_snapshot(snapshot, expected)

        assert (sentinel.read_text(encoding="utf-8")) == ("preserve\n")

    def test_failed_materialization_removes_its_staging_directory(self, tmp_path: Path) -> None:
        """An injected write failure removes the private staging directory."""
        root = tmp_path
        snapshot = workspace_capture.workspace_snapshot([("source", self._source(root))])
        with (
            mock.patch.object(workspace_staging, "ROOT", root),
            mock.patch.object(
                workspace_staging,
                "_write_snapshot_file",
                side_effect=OSError("write failed"),
            ),
            pytest.raises(OSError, match="write failed"),
        ):
            workspace_staging.stage_workspace_snapshot(snapshot)

        assert (list(root.rglob(".stage-*"))) == ([])

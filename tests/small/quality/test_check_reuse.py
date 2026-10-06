# SPDX-License-Identifier: GPL-2.0-only
"""Receipt reuse through mocked OCI and checker boundaries."""

from __future__ import annotations

import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli.environment import image_store
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.quality import checks
from fplinux_cli.quality import runtime as quality_runtime
from fplinux_cli.workspace.capture import WorkspaceFile, WorkspaceSnapshot

from tests.small.quality.check_fixtures import _FailingReporter, _RecordingReporter


class MockedCheckReceiptOrchestrationTests(unittest.TestCase):
    """Exercise real receipt state with mocked OCI and checker boundaries."""

    @staticmethod
    def _snapshot(*, c_source: bytes = b"int app;\n") -> WorkspaceSnapshot:
        files = (
            WorkspaceFile("README.md", b"documentation\n", 0o644),
            WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
            WorkspaceFile(
                "alpine/aports/demo-consumer/app.c",
                c_source,
                0o644,
            ),
        )
        return WorkspaceSnapshot(files, "a" * 64)

    @staticmethod
    def _guarded_boundary(
        name: str,
        normal_result: object,
        *,
        exact_hit_guard: bool,
    ) -> mock.Mock:
        """Fail if an exact receipt hit crosses a boundary that should remain untouched."""
        return mock.Mock(
            side_effect=(
                AssertionError(f"exact check hit must not {name}") if exact_hit_guard else None
            ),
            return_value=None if exact_hit_guard else normal_result,
        )

    def _check_boundary_patches(  # noqa: PLR0913
        self,
        *,
        root: Path,
        reporter: _RecordingReporter,
        snapshot: WorkspaceSnapshot,
        stage_workspace: mock.Mock,
        discard_workspace: mock.Mock,
        exact_hit_guard: bool,
    ) -> tuple[Any, ...]:
        """Describe the controlled OCI, image, and workspace boundaries for a scenario."""
        return (
            mock.patch.object(checks, "ROOT", root),
            mock.patch.object(image_store, "ROOT", root),
            mock.patch("fplinux_cli.quality.checks.RunReporter.create", return_value=reporter),
            mock.patch.object(
                quality_runtime,
                "kern_available",
                new=self._guarded_boundary(
                    "inspect Kern",
                    normal_result=True,
                    exact_hit_guard=exact_hit_guard,
                ),
            ),
            mock.patch.object(
                quality_runtime,
                "require_kern",
                new=self._guarded_boundary(
                    "require Kern",
                    "kern",
                    exact_hit_guard=exact_hit_guard,
                ),
            ),
            mock.patch.object(
                checks,
                "load_container_lock",
                return_value={
                    "oci": {
                        "repository": "localhost/fplinux-build",
                        "platform": "linux/amd64",
                    }
                },
            ),
            mock.patch.object(
                quality_runtime,
                "current_image_state",
                new=self._guarded_boundary(
                    "inspect an image",
                    ImageState("b" * 64, "c" * 64, "c" * 64),
                    exact_hit_guard=exact_hit_guard,
                ),
            ),
            mock.patch.object(checks, "kern_environment", return_value={}),
            mock.patch.object(quality_runtime, "kern_environment", return_value={}),
            mock.patch.object(
                checks,
                "container_image_recipe_digest",
                return_value="b" * 64,
            ),
            mock.patch.object(
                checks,
                "check_orchestration_recipe_digest",
                return_value="d" * 64,
            ),
            mock.patch.object(
                checks,
                "quality_workspace_snapshot",
                return_value=snapshot,
            ),
            mock.patch.object(
                checks,
                "stage_quality_workspace_snapshot",
                new=(
                    self._guarded_boundary(
                        "stage a workspace",
                        None,
                        exact_hit_guard=True,
                    )
                    if exact_hit_guard
                    else stage_workspace
                ),
            ),
            mock.patch.object(
                checks,
                "discard_staged_quality_workspace_snapshot",
                new=discard_workspace,
            ),
            mock.patch.object(
                quality_runtime,
                "setup",
                new=self._guarded_boundary(
                    "set up an image",
                    None,
                    exact_hit_guard=exact_hit_guard,
                ),
            ),
        )

    def _execute_check(
        self,
        scopes: list[str],
        *,
        no_cache: bool,
        patches: tuple[Any, ...],
        jobs: int,
    ) -> None:
        """Run the production check entry point with the scenario's controlled boundaries."""
        with ExitStack() as stack:
            for boundary in patches:
                stack.enter_context(boundary)
            checks.check(scopes, no_cache=no_cache, jobs=jobs)

    def _run(  # noqa: PLR0913
        self,
        root: Path,
        workspace: Path,
        snapshot: WorkspaceSnapshot,
        scopes: list[str],
        commands: list[list[str]],
        *,
        no_cache: bool = False,
        reporter_type: type[_RecordingReporter] = _RecordingReporter,
        exact_hit_guard: bool = False,
        jobs: int = 1,
    ) -> None:
        logs = root / f"logs-{len(commands)}"
        logs.mkdir(exist_ok=True)
        reporter = reporter_type(logs, commands)
        stage_workspace = mock.Mock(return_value=workspace)
        discard_workspace = mock.Mock()
        patches = self._check_boundary_patches(
            root=root,
            reporter=reporter,
            snapshot=snapshot,
            stage_workspace=stage_workspace,
            discard_workspace=discard_workspace,
            exact_hit_guard=exact_hit_guard,
        )
        try:
            self._execute_check(scopes, no_cache=no_cache, patches=patches, jobs=jobs)
        except RuntimeError:
            if not exact_hit_guard:
                discard_workspace.assert_called_once_with(snapshot, workspace)
            raise
        if exact_hit_guard:
            discard_workspace.assert_not_called()
        else:
            discard_workspace.assert_called_once_with(snapshot, workspace)

    def test_exact_success_hit_skips_workspace_and_checker(self) -> None:
        """Return a verified scope hit before workspace materialization."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()

            self._run(root, workspace, snapshot, ["c"], commands)
            self.assertEqual(len(commands), 1)

            self._run(
                root,
                workspace,
                snapshot,
                ["c"],
                commands,
                exact_hit_guard=True,
            )
            self.assertEqual(len(commands), 1)

    def test_kernel_hit_is_reused_with_a_different_worker_limit(self) -> None:
        """Reuse a kernel success when only --jobs changes between check runs."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = WorkspaceSnapshot(
                (
                    WorkspaceFile(
                        "scripts/fplinux_cli/quality/kernel/analysis.py", b"checker\n", 0o644
                    ),
                ),
                "a" * 64,
            )

            self._run(root, workspace, snapshot, ["kernel"], commands, jobs=1)
            self.assertEqual(len(commands), 2)

            self._run(
                root,
                workspace,
                snapshot,
                ["kernel"],
                commands,
                exact_hit_guard=True,
                jobs=2,
            )
            self.assertEqual(len(commands), 2)

    def test_no_cache_bypasses_an_exact_outer_receipt(self) -> None:
        """Execute the checker when the caller explicitly ignores receipts."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()
            self._run(root, workspace, snapshot, ["c"], commands)
            self._run(root, workspace, snapshot, ["c"], commands, no_cache=True)
            self.assertEqual(len(commands), 2)

    def test_only_missing_scope_runs_in_a_mixed_selection(self) -> None:
        """Avoid rerunning a hit when another selected scope is missing."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()
            self._run(root, workspace, snapshot, ["c"], commands)
            self._run(root, workspace, snapshot, ["c", "docs"], commands)
            self.assertEqual(
                [
                    command[command.index("/workspace/scripts/check.py") + 1 :]
                    for command in commands
                ],
                [["c"], ["docs"]],
            )

    def test_changed_c_bytes_are_a_cold_miss(self) -> None:
        """Invalidate the C result when one checked source byte changes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            self._run(root, workspace, self._snapshot(), ["c"], commands)
            self._run(
                root,
                workspace,
                self._snapshot(c_source=b"int changed;\n"),
                ["c"],
                commands,
            )
            self.assertEqual(len(commands), 2)

    def test_failed_forced_rerun_keeps_last_good_success(self) -> None:
        """A failed forced rerun leaves the previous exact success usable."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / ".cache").mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            commands: list[list[str]] = []
            snapshot = self._snapshot()
            self._run(root, workspace, snapshot, ["c"], commands)
            with self.assertRaisesRegex(RuntimeError, "scope failed"):
                self._run(
                    root,
                    workspace,
                    snapshot,
                    ["c"],
                    commands,
                    no_cache=True,
                    reporter_type=_FailingReporter,
                )
            self._run(
                root,
                workspace,
                snapshot,
                ["c"],
                commands,
                exact_hit_guard=True,
            )
            self.assertEqual(len(commands), 2)


if __name__ == "__main__":
    unittest.main()

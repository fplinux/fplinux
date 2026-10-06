# SPDX-License-Identifier: GPL-2.0-only
"""Host fast paths and controlled checker execution policy."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.quality import checks
from fplinux_cli.quality import runtime as quality_runtime
from fplinux_cli.quality.receipts import check_scope_receipt_recipe
from fplinux_cli.quality.scopes import resolve_check_scopes

from tests.small.quality.check_fixtures import _RecordingReporter


class CheckScopeSelectionTests(unittest.TestCase):
    """Keep scope selection stable and independent of argument order."""

    def test_selection_is_deduplicated_in_canonical_order(self) -> None:
        """Deduplicate selections and ignore their command-line order."""
        self.assertEqual(
            resolve_check_scopes(["kernel", "python", "kernel", "repository"]),
            ("repository", "python", "kernel"),
        )


class RepositoryFastPathTests(unittest.TestCase):
    """Keep the repository-only check completely on the host."""

    def test_repository_check_returns_before_runtime_or_workspace(self) -> None:
        """Return after the host check without requiring a container or snapshot."""
        reporter = mock.Mock()
        with (
            mock.patch("fplinux_cli.quality.checks.RunReporter.create", return_value=reporter),
            mock.patch.object(checks, "check_git_diff") as git_diff,
            mock.patch.object(
                quality_runtime,
                "kern_available",
                side_effect=AssertionError("repository check must not inspect Kern"),
            ),
            mock.patch.object(
                checks,
                "quality_workspace_snapshot",
                side_effect=AssertionError("repository check must not snapshot a workspace"),
            ),
        ):
            checks.check(["repository"], profile="microsd-uboot")
        git_diff.assert_called_once_with(reporter)
        reporter.finish.assert_called_once_with()


class KernelExecutionLimitTests(unittest.TestCase):
    """Keep kernel worker limits out of the prepare container command."""

    def test_kernel_limit_reaches_only_the_analysis_command(self) -> None:
        """Pass --jobs to kernel analysis while prepare keeps the same arguments."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "cache"
            cache.mkdir()
            workspace = root / "workspace"
            workspace.mkdir()
            analyzer_cache = {name: root / name for name in ("analysis", "downloads", "linux")}
            for path in analyzer_cache.values():
                path.mkdir()
            commands: list[list[str]] = []
            recipe = check_scope_receipt_recipe(
                "kernel",
                "a" * 64,
                image_generation="b" * 64,
                orchestration_recipe="c" * 64,
                profile="microsd-uboot",
            )

            def run_with_jobs(jobs: int) -> None:
                with mock.patch.object(checks, "kern_environment", return_value={}):
                    checks._run_missing_checks(  # noqa: SLF001 -- execution boundary.
                        reporter=_RecordingReporter(  # type: ignore[arg-type]
                            root / f"logs-{jobs}", commands
                        ),
                        cache=cache,
                        missing=("kernel",),
                        analyzer_cache=analyzer_cache,
                        workspace=workspace,
                        kern="kern",
                        image="localhost/fplinux-build:locked",
                        recipes={"kernel": recipe},
                        profile="microsd-uboot",
                        build_type="release",
                        jobs=jobs,
                    )

            run_with_jobs(1)
            run_with_jobs(2)

            first_prepare, first_analysis, second_prepare, second_analysis = commands
            self.assertIn(f"{root / 'logs-1/containers'}:/logs", first_prepare)
            self.assertIn(f"{workspace}:/workspace:ro", first_prepare)
            self.assertIn(f"{analyzer_cache['downloads']}:/cache/downloads", first_prepare)
            self.assertIn(f"{analyzer_cache['linux']}:/cache/linux", first_prepare)
            self.assertIn(f"{analyzer_cache['analysis']}:/cache/analysis", first_analysis)
            self.assertIn(f"{analyzer_cache['linux']}:/cache/linux:ro", first_analysis)
            self.assertEqual(
                first_prepare[-5:],
                ["prepare", "--profile", "microsd-uboot", "--build-type", "release"],
            )
            self.assertEqual(
                first_analysis[-7:],
                ["check", "--jobs", "1", "--profile", "microsd-uboot", "--build-type", "release"],
            )
            self.assertEqual(
                second_prepare[-5:],
                ["prepare", "--profile", "microsd-uboot", "--build-type", "release"],
            )
            self.assertEqual(
                second_analysis[-7:],
                ["check", "--jobs", "2", "--profile", "microsd-uboot", "--build-type", "release"],
            )


if __name__ == "__main__":
    unittest.main()

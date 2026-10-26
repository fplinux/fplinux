# SPDX-License-Identifier: GPL-2.0-only
"""Host fast paths and controlled checker execution policy."""

from __future__ import annotations

import argparse
import hashlib
import io
import tarfile
import tempfile
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli.build.kernel.state import ensure_linux_base
from fplinux_cli.common import load_toml
from fplinux_cli.environment.state import EnvironmentState, publish_environment_state
from fplinux_cli.manifests.source_catalog import resolve_source, source_build_metadata
from fplinux_cli.quality import checks
from fplinux_cli.quality import runtime as quality_runtime
from fplinux_cli.quality.receipts import (
    check_scope_receipt_recipe,
    publish_success_receipt,
    receipt_matches,
)
from fplinux_cli.quality.scopes import resolve_check_scopes
from fplinux_cli.reporting.run import RunReporter, Stage
from fplinux_cli.workspace.capture import workspace_snapshot

from tests.fixtures import linux_inputs
from tests.small.quality.check_fixtures import _RecordingReporter

if TYPE_CHECKING:
    from urllib.request import Request


class CheckScopeSelectionTests:
    """Keep scope selection stable and independent of argument order."""

    def test_selection_is_deduplicated_in_canonical_order(self) -> None:
        """Deduplicate selections and ignore their command-line order."""
        assert (resolve_check_scopes(["kernel", "python", "kernel", "repository"])) == (
            ("repository", "python", "kernel")
        )

    def test_default_selection_includes_source_tests_and_kernel_checks(self) -> None:
        """Omitting scopes retains the complete gate, including the full test workload."""
        assert resolve_check_scopes([]) == (
            "repository",
            "source",
            "metadata",
            "docs",
            "spelling",
            "secrets",
            "licenses",
            "python",
            "shell",
            "alpine",
            "c",
            "tests",
            "kernel",
        )


class AnalyzerAndTestExecutionTests:
    """Exercise scope routing and receipts with controlled external container processes."""

    @pytest.mark.parametrize(
        ("selected", "source_scopes", "test_tiers"),
        [
            (("python",), ["python"], []),
            (
                ("tests",),
                [],
                ["small", "host_process", "host_tool", "artifact", "public_workflow"],
            ),
            (
                ("python", "tests"),
                ["python"],
                ["small", "host_process", "host_tool", "artifact", "public_workflow"],
            ),
        ],
        ids=("python-analyzers-only", "all-test-tiers-only", "analyzers-and-all-test-tiers"),
    )
    def test_selected_scopes_route_to_their_workload_and_earn_receipts(
        self,
        tmp_path: Path,
        selected: tuple[str, ...],
        source_scopes: list[str],
        test_tiers: list[str],
    ) -> None:
        """Python runs no pytest; tests runs all tiers without entering the source checker."""
        workspace = tmp_path / "workspace"
        (workspace / "tests").mkdir(parents=True)
        commands: list[list[str]] = []
        recipes = {
            scope: check_scope_receipt_recipe(
                scope,
                "a" * 64,
                environment_generation="b" * 64,
                orchestration_recipe="c" * 64,
            )
            for scope in selected
        }
        cache = tmp_path / "cache"
        with mock.patch.object(quality_runtime, "kern_environment", return_value={}):
            checks._run_missing_checks(  # noqa: SLF001 -- controlled execution boundary.
                reporter=_RecordingReporter(tmp_path / "logs", commands),  # type: ignore[arg-type]
                cache=cache,
                missing=selected,
                analyzer_cache={},
                workspace=workspace,
                kern="kern",
                rootfs=Path("/environment/rootfs"),
                recipes=recipes,
                profile=None,
                build_type="release",
                jobs=1,
            )

        workloads = [command[command.index("--") + 1 :] for command in commands]
        analyzer_workloads = [
            command
            for command in workloads
            if command[:3] == ["python3", "-m", "fplinux_cli.quality.source"]
        ]
        assert analyzer_workloads == (
            [["python3", "-m", "fplinux_cli.quality.source", *source_scopes]]
            if source_scopes
            else []
        )
        pytest_workloads = [
            command for command in workloads if command[:3] == ["python3", "-m", "pytest"]
        ]
        assert [command[-1] for command in pytest_workloads] == [
            f"tests/{tier}" for tier in test_tiers
        ]
        assert len(workloads) == len(analyzer_workloads) + len(pytest_workloads)
        assert all(receipt_matches(cache, recipe) for recipe in recipes.values())

    def test_final_container_tier_failure_keeps_python_success_and_previous_tests_receipt(
        self, tmp_path: Path
    ) -> None:
        """Earlier tiers cannot certify tests when the final container workload fails."""
        workspace = tmp_path / "workspace"
        (workspace / "tests").mkdir(parents=True)
        python = check_scope_receipt_recipe(
            "python", "a" * 64, environment_generation="b" * 64, orchestration_recipe="c" * 64
        )
        previous_tests = replace(python, scope="tests")
        changed_tests = replace(previous_tests, closure_digest="d" * 64)
        cache = tmp_path / "cache"
        publish_success_receipt(cache, previous_tests)
        analyzer_commands: list[list[str]] = []
        test_commands: list[list[str]] = []

        def container_workload(
            _stage: object, _workspace: Path, command: list[str], **_kwargs: object
        ) -> None:
            test_commands.append(command)
            if command[-1] == "tests/public_workflow":
                raise SystemExit(1)

        with (
            mock.patch.object(quality_runtime, "kern_environment", return_value={}),
            mock.patch.object(
                quality_runtime, "run_quality_command", side_effect=container_workload
            ),
            pytest.raises(SystemExit) as raised,
        ):
            checks._run_missing_checks(  # noqa: SLF001 -- receipt publishing boundary.
                reporter=_RecordingReporter(  # type: ignore[arg-type]
                    tmp_path / "logs", analyzer_commands
                ),
                cache=cache,
                missing=("python", "tests"),
                analyzer_cache={},
                workspace=workspace,
                kern="kern",
                rootfs=Path("/environment/rootfs"),
                recipes={"python": python, "tests": changed_tests},
                profile=None,
                build_type="release",
                jobs=1,
            )

        assert raised.value.code == 1
        assert len(analyzer_commands) == 1
        assert [command[-1] for command in test_commands] == [
            "tests/small",
            "tests/host_process",
            "tests/host_tool",
            "tests/artifact",
            "tests/public_workflow",
        ]
        assert receipt_matches(cache, python)
        assert receipt_matches(cache, previous_tests)
        assert not receipt_matches(cache, changed_tests)


class RepositoryFastPathTests:
    """Keep the repository-only check completely on the host."""

    def test_repository_check_returns_before_runtime_or_workspace(self) -> None:
        """Return after the host check without requiring a container or snapshot."""
        reporter = mock.Mock()
        with (
            mock.patch("fplinux_cli.quality.checks.RunReporter.create", return_value=reporter),
            mock.patch.object(checks, "check_git_diff") as git_diff,
            mock.patch.object(
                checks,
                "require_environment",
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


class KernelInputAcquisitionTests:
    """Acquire captured originals before their offline preparation process."""

    def test_first_kernel_check_acquires_captured_sources_before_offline_preparation(
        self, tmp_path: Path
    ) -> None:
        """Acquire selected originals before preparation, then reuse the success on the host."""
        archive = io.BytesIO()
        makefile = b"VERSION = fixture\n"
        with tarfile.open(fileobj=archive, mode="w:gz") as bundle:
            member = tarfile.TarInfo("linux-fixture/Makefile")
            member.size = len(makefile)
            bundle.addfile(member, io.BytesIO(makefile))
        original = archive.getvalue()
        checksum = hashlib.sha256(original).hexdigest()
        workspace = tmp_path / "captured"
        linux_inputs.platform(workspace, "ums9117", source_lock="linux-captured", arch="arm")
        linux_inputs.platform(workspace, "sc9820e", source_lock="linux-skipped", arch="arm")
        linux_inputs.target(workspace, "first", platform_name="ums9117")
        linux_inputs.target(workspace, "second", platform_name="ums9117")
        linux_inputs.target(workspace, "other-profile", platform_name="sc9820e")
        catalog = (
            Path(__file__).parent / "fixtures/kernel-inputs/source-catalog.txt.in"
        ).read_text(encoding="utf-8") % {"checksum": checksum, "size": len(original)}
        linux_inputs.write(workspace, "sources.lock.toml", catalog)
        snapshot = workspace_snapshot(
            [
                (path.relative_to(workspace).as_posix(), path)
                for path in workspace.rglob("*")
                if path.is_file()
            ]
        )
        root = tmp_path / "live"
        cache = root / ".cache"
        environment = EnvironmentState("b" * 64, "c" * 64, "d" * 64)
        publish_environment_state(cache, environment)
        requests: list[str] = []
        commands: list[list[str]] = []

        def serve_original(request: Request, **_kwargs: object) -> io.BytesIO:
            requests.append(request.full_url)
            assert request.full_url == "https://example.invalid/selected-linux.tar.gz"
            return io.BytesIO(original)

        def container_workload(_stage: Stage, command: list[str], **_kwargs: object) -> None:
            assert command[command.index("--network") + 1] == "none"
            assert f"{cache / 'downloads'}:/cache/downloads:ro" in command
            commands.append(command)
            workload = command[command.index("--") + 1 :]
            if workload[3] == "prepare":
                source = resolve_source(workspace, "linux-captured")
                local_archive = source.acquire(cache, offline=True)
                assert local_archive.read_bytes() == original
                base = ensure_linux_base(
                    cache,
                    source_build_metadata(
                        source.id, load_toml(workspace / "sources.lock.toml")[source.id]
                    ),
                    archive=local_archive,
                )
                assert (base.source / "Makefile").read_bytes() == makefile

        def reporter_for_run(*_args: object, **_kwargs: object) -> RunReporter:
            return RunReporter("check", tmp_path / f"logs-{len(commands)}", "logs", verbose=False)

        with (
            mock.patch.object(checks, "ROOT", root),
            mock.patch.object(checks, "load_environment_lock", return_value={}),
            mock.patch.object(
                checks, "environment_recipe_digest", return_value=environment.recipe
            ),
            mock.patch.object(checks, "check_orchestration_recipe_digest", return_value="e" * 64),
            mock.patch.object(checks, "quality_workspace_snapshot", return_value=snapshot),
            mock.patch.object(
                checks, "stage_quality_workspace_snapshot", return_value=workspace
            ) as stage_workspace,
            mock.patch.object(checks, "discard_staged_quality_workspace_snapshot"),
            mock.patch.object(checks, "kern_environment", return_value={}),
            mock.patch.object(
                checks,
                "require_environment",
                return_value=("kern", root / "rootfs", environment),
            ) as require_environment,
            mock.patch.object(RunReporter, "create", side_effect=reporter_for_run),
            mock.patch.object(Stage, "run", autospec=True, side_effect=container_workload),
            mock.patch("urllib.request.urlopen", side_effect=serve_original),
        ):
            checks.check(["kernel"], profile="microsd-uboot")
            assert requests == ["https://example.invalid/selected-linux.tar.gz"]
            assert len(commands) == 2
            with (
                mock.patch.object(
                    checks, "acquire_inputs", side_effect=AssertionError("acquisition")
                ),
                mock.patch.object(Stage, "run", side_effect=AssertionError("Kern")),
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
            ):
                checks.check(["kernel"], profile="microsd-uboot", jobs=2)
            stage_workspace.assert_called_once_with(snapshot)
            require_environment.assert_called_once()
        assert len(commands) == 2


class KernelExecutionLimitTests:
    """Keep container memory explicit and worker limits on the analysis process."""

    @pytest.mark.parametrize(
        ("failed_stage", "jobs", "status", "has_serial_advice"),
        [
            pytest.param("kernel-prepare", 3, 137, False, id="serial-preparation"),
            pytest.param("kernel-analysis", 3, 137, True, id="parallel-analysis"),
            pytest.param("kernel-analysis", 1, 137, False, id="serial-analysis"),
            pytest.param("kernel-analysis", 3, 1, False, id="other-analysis-failure"),
        ],
    )
    def test_kernel_memory_failure_reports_only_applicable_controls(  # noqa: PLR0913 -- phase, budget and outcome stay explicit.
        self,
        tmp_path: Path,
        capsys: pytest.CaptureFixture[str],
        failed_stage: str,
        jobs: int,
        status: int,
        *,
        has_serial_advice: bool,
    ) -> None:
        """Only parallel analysis can recommend serial contexts; preparation is already serial."""
        reporter = RunReporter("check", tmp_path / "logs", "test", verbose=False)
        failure = SystemExit(status)

        def fail_workload(stage: Stage, command: list[str], **_kwargs: object) -> None:
            assert command[command.index("--memory") + 1] == "8192m"
            if stage.name == failed_stage:
                stage.write(
                    b"kern: the workload was killed by the kernel's OOM killer "
                    b"against this box's own memory cap.\n"
                )
                raise failure

        with (
            mock.patch.object(checks, "kern_environment", return_value={}),
            mock.patch.object(Stage, "run", autospec=True, side_effect=fail_workload),
            pytest.raises(SystemExit) as raised,
        ):
            checks._run_missing_checks(  # noqa: SLF001 -- controlled execution boundary.
                reporter=reporter,
                cache=tmp_path / "cache",
                missing=("kernel",),
                analyzer_cache={
                    name: tmp_path / name for name in ("analysis", "downloads", "linux")
                },
                workspace=tmp_path / "workspace",
                kern="kern",
                rootfs=Path("/environment/rootfs"),
                recipes={},
                profile="microsd-uboot",
                build_type="debug",
                jobs=jobs,
            )

        assert raised.value is failure
        stderr = capsys.readouterr().err
        assert stderr.count("Kern memory limit reached (8192 MiB).") == int(status == 137)
        assert (
            "Use check --jobs 1 to run kernel contexts serially." in stderr
        ) == has_serial_advice
        assert "--memory-mib" not in stderr

    def test_kernel_memory_and_worker_limits_reach_the_correct_processes(self) -> None:
        """Give both containers a compiler memory budget and only analysis --jobs."""
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
                environment_generation="b" * 64,
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
                        rootfs=Path("/environment/rootfs"),
                        recipes={"kernel": recipe},
                        profile="microsd-uboot",
                        build_type="release",
                        jobs=jobs,
                    )

            run_with_jobs(1)
            run_with_jobs(2)

            first_prepare, first_analysis, second_prepare, second_analysis = commands
            assert (f"{root / 'logs-1/containers'}:/logs") in (first_prepare)
            assert (f"{workspace}:/workspace:ro") in (first_prepare)
            assert (f"{analyzer_cache['downloads']}:/cache/downloads:ro") in (first_prepare)
            assert (f"{analyzer_cache['linux']}:/cache/linux") in (first_prepare)
            assert (f"{analyzer_cache['analysis']}:/cache/analysis") in (first_analysis)
            assert (f"{analyzer_cache['linux']}:/cache/linux:ro") in (first_analysis)
            parser = argparse.ArgumentParser()
            parser.add_argument("phase", choices=("prepare", "check"))
            parser.add_argument("--profile", required=True)
            parser.add_argument("--build-type", required=True)
            parser.add_argument("--jobs", type=int)
            for command, phase, jobs in (
                (first_prepare, "prepare", None),
                (first_analysis, "check", 1),
                (second_prepare, "prepare", None),
                (second_analysis, "check", 2),
            ):
                container_arguments = command[3 : command.index("--")]
                assert container_arguments.count("--memory") == 1
                assert container_arguments[container_arguments.index("--memory") + 1] == "8192m"
                workload = command[command.index("--") + 1 :]
                assert workload[:3] == ["python3", "-m", "fplinux_cli.quality.kernel"]
                arguments = workload[3:]
                assert arguments.count("--jobs") == (0 if jobs is None else 1)
                selected = parser.parse_args(arguments)
                assert vars(selected) == {
                    "phase": phase,
                    "profile": "microsd-uboot",
                    "build_type": "release",
                    "jobs": jobs,
                }

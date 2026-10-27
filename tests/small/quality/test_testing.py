# SPDX-License-Identifier: GPL-2.0-only
"""Selected-test orchestration with a stubbed external Kern process boundary."""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import sys
import tempfile
import time
from dataclasses import replace
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.environment import runtime as environment_runtime
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.manifests.source_catalog import ResolvedSource
from fplinux_cli.quality import checks, host_testing, runtime, testing
from fplinux_cli.quality.receipts import (
    CheckReceiptRecipe,
    check_orchestration_recipe_digest,
    check_scope_receipt_recipe,
    publish_success_receipt,
    receipt_matches,
    receipt_path,
)
from fplinux_cli.reporting import run as output
from fplinux_cli.workspace import capture as workspace
from fplinux_cli.workspace import quality_inputs
from fplinux_cli.workspace import staging as workspace_staging

from tests import ROOT


@pytest.mark.parametrize("original_state", ["missing", "mismatched", "ready"])
def test_emmc_workload_requires_exact_local_originals_without_downloads(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, original_state: str
) -> None:
    """Missing originals stop test execution; matching saved bytes reach the workload."""
    from fplinux_cli.build.host_tools import phone_data  # noqa: PLC0415 -- workload-only input.

    module = tmp_path / "tests/artifact/test_emmc_files.py"
    module.parent.mkdir(parents=True)
    module.touch()
    original = ResolvedSource(
        "python_ext4",
        "fixture",
        "ext4.whl",
        "https://example.invalid/ext4.whl",
        "sha256",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        3,
        "downloads/sources/python_ext4/ext4.whl",
    )
    saved = tmp_path / ".cache/downloads/sources/python_ext4/ext4.whl"
    if original_state != "missing":
        saved.parent.mkdir(parents=True)
        saved.write_bytes(b"abc" if original_state == "ready" else b"bad")
    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setattr(phone_data, "preparation_sources", lambda _root: (original,))
    reporter = output.RunReporter("test", tmp_path / "logs", "test", verbose=False)
    commands: list[list[str]] = []

    def run_workload(
        _stage: output.Stage, _workspace: Path, command: list[str], **options: object
    ) -> None:
        assert options["downloads"] == tmp_path / ".cache/downloads"
        commands.append(command)

    with (
        mock.patch.object(runtime, "run_quality_command", side_effect=run_workload),
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
    ):
        if original_state != "ready":
            with pytest.raises(SystemExit, match=r"ext4.whl.*setup --all"):
                testing.run_test_workload(
                    reporter,
                    tmp_path,
                    kern="kern",
                    rootfs=tmp_path / "rootfs",
                    names=[],
                    tier="artifact",
                )
            assert commands == []
            return
        testing.run_test_workload(
            reporter, tmp_path, kern="kern", rootfs=tmp_path / "rootfs", names=[], tier="artifact"
        )
    assert len(commands) == 1
    assert commands[0][-1] == "tests/artifact"
    assert saved.read_bytes() == b"abc"


class SelectedTestRunTests:
    """A test result owns logs, never a full quality-check success receipt."""

    @pytest.mark.parametrize(
        ("names", "tier"),
        [
            pytest.param([], "artifact", id="tier"),
            pytest.param(["tests/artifact"], None, id="directory"),
            pytest.param(["tests.artifact.test_example"], None, id="module"),
            pytest.param(["tests/artifact/test_example.py::test_example"], None, id="node"),
        ],
    )
    def test_artifact_selection_forms_allow_memory_for_images_and_tools(
        self, tmp_path: Path, names: list[str], tier: str | None
    ) -> None:
        """Each selection gives Kern room for image files, pytest and native image tools."""
        module = tmp_path / "workspace/tests/artifact/test_example.py"
        module.parent.mkdir(parents=True)
        module.touch()
        reporter = output.RunReporter(
            "test", tmp_path / "logs", str(tmp_path / "logs"), verbose=False
        )
        with (
            mock.patch.object(runtime, "kern_environment", return_value={}),
            # Kern resource arguments are the external boundary under this test.
            mock.patch.object(output.Stage, "run", autospec=True) as kern_process,
        ):
            testing.run_test_workload(
                reporter,
                tmp_path / "workspace",
                kern="kern",
                rootfs=Path("/environment/rootfs"),
                names=names,
                tier=tier,
            )

        kern_process.assert_called_once()
        command = kern_process.call_args.args[1]
        container_arguments = command[: command.index("--")]
        assert container_arguments.count("--memory") == 1
        assert container_arguments[container_arguments.index("--memory") + 1] == "2048m"
        tmpfs = container_arguments[container_arguments.index("--tmpfs") + 1]
        assert tmpfs == "/tmp:1g"  # noqa: S108 -- container tmpfs.

    @pytest.mark.parametrize("tier", [None, "host_tool"])
    def test_host_tool_selections_and_discovery_share_time_for_native_builds(
        self, tier: str | None, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """More native selections share one declared budget instead of multiplying it."""
        monkeypatch.setattr(
            testing,
            "TEST_TIERS",
            {
                "small": 7,
                "host_process": 11,
                "host_tool": 19,
                "artifact": 13,
                "public_workflow": 5,
            },
        )
        workloads = testing.pytest_commands([], tier=tier)
        full = {label: timeout for label, _command, timeout in workloads}
        assert full["host_tool"] == 19
        if tier is None:
            assert {label: timeout for label, timeout in full.items() if label != "host_tool"} == {
                "small": 7,
                "host_process": 11,
                "artifact": 13,
                "public_workflow": 5,
            }
        for names in (
            ["tests.host_tool.gui.test_clocks_screens"],
            [
                "tests.host_tool.gui.test_apidemo_screens",
                "tests.host_tool.gui.test_terminal_screens",
                "tests.host_tool.gui.test_calculator_screens",
                "tests.host_tool.gui.test_clocks_screens",
                "tests.host_tool.gui.test_ui_client_frames",
            ],
        ):
            [(_label, _command, selected_timeout)] = testing.pytest_commands(names)
            assert selected_timeout == 19

    def test_directory_selection_uses_captured_workspace(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A directory captured earlier remains selectable after its live path disappears."""
        captured = tmp_path / "captured"
        (captured / "tests/small/quality").mkdir(parents=True)
        live = tmp_path / "live"
        live.mkdir()
        monkeypatch.chdir(live)

        [(_label, command, timeout)] = testing.pytest_commands(
            ["tests/small/quality"], workspace=captured
        )

        assert command[-1] == "tests/small/quality"
        assert timeout == 90

    def test_live_directory_missing_from_capture_is_not_selected(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A newly created live directory cannot enter an immutable selected workload."""
        captured = tmp_path / "captured"
        captured.mkdir()
        live = tmp_path / "live"
        (live / "tests/small/quality").mkdir(parents=True)
        monkeypatch.chdir(live)

        with pytest.raises(SystemExit, match="test directory not found in captured workspace"):
            testing.pytest_commands(["tests/small/quality"], workspace=captured)

    def test_named_selection_counts_each_tiers_time_budget_once(self) -> None:
        """Names spanning tiers get their combined budget; more names in a tier add nothing."""

        def selected_budget(*names: str) -> int:
            [(_label, _command, timeout)] = testing.pytest_commands(list(names))
            return timeout

        small = selected_budget("tests.small.environment.test_common.FirstCase")
        host_process = selected_budget("tests.host_process.test_process")
        assert small == 90
        assert host_process == 300
        assert (
            selected_budget(
                "tests.small.environment.test_common.FirstCase",
                "tests.small.environment.test_common.SecondCase",
                "tests.host_process.test_process",
            )
            == small + host_process
        )
        assert (
            selected_budget(
                "tests/small/test_example.py::ExampleTests::test_first",
                "tests/host_process/test_example.py::test_example[case]",
            )
            == 390
        )

    @pytest.mark.parametrize(
        ("status", "failure", "exit_code"),
        [
            ("success", None, 0),
            ("failed", SystemExit(1), 1),
            ("interrupted", KeyboardInterrupt(), 130),
        ],
    )
    def test_all_outcomes_preserve_check_receipts_and_release_workspace(
        self, status: str, failure: BaseException | None, exit_code: int
    ) -> None:
        """Success, failure and interruption preserve check evidence and release staged sources."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / ".cache"
            recipes = [
                CheckReceiptRecipe(scope, "a" * 64, "b" * 64, "c" * 64)
                for scope in ("python", "tests")
            ]
            for recipe in recipes:
                publish_success_receipt(cache, recipe)
            original = {
                receipt_path(cache, recipe): receipt_path(cache, recipe).read_bytes()
                for recipe in recipes
            }
            fixture = root / "tests/small/test_example.py"
            fixture.parent.mkdir(parents=True)
            fixture.write_text("# fixture\n")
            with mock.patch.object(common, "ROOT", root):
                snapshot = workspace.workspace_snapshot([("tests/small/test_example.py", fixture)])
            with (
                mock.patch.object(workspace_staging, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(
                    quality_inputs, "quality_workspace_snapshot", return_value=snapshot
                ),
                mock.patch.object(
                    environment_runtime,
                    "require_environment",
                    return_value=(
                        "kern",
                        root / ".cache/environment/rootfs",
                        EnvironmentState("b" * 64, "c" * 64, "d" * 64),
                    ),
                ),
                # Kern itself is outside this unit boundary; selection has real process tests.
                mock.patch.object(
                    runtime,
                    "run_quality_command",
                    side_effect=failure,
                ) as container_workload,
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                if failure is not None:
                    with pytest.raises(SystemExit) as raised:
                        output.run_entrypoint(
                            lambda: testing.run_tests(["tests.small.test_example"])
                        )
                    assert (raised.value.code) == (exit_code)
                else:
                    output.run_entrypoint(lambda: testing.run_tests(["tests.small.test_example"]))
            container_workload.assert_called_once()
            assert {path: path.read_bytes() for path in original} == original
            assert set((cache / "check-results").rglob("*.json")) == set(original)
            assert (list((cache / "quality-workspaces").iterdir())) == ([])
            runs = list((cache / "logs/test").glob("*/run.json"))
            assert (len(runs)) == (1)
            assert (json.loads(runs[0].read_text())["status"]) == (status)

    def test_host_and_container_use_one_host_process_deadline(self, tmp_path: Path) -> None:
        """A host subprocess receives only the time left by the preceding container work."""
        module = tmp_path / "tests/host_process/cache/test_prune_rootfs_ownership.py"
        module.parent.mkdir(parents=True)
        module.touch()
        reporter = output.RunReporter(
            "test", tmp_path / "logs", str(tmp_path / "logs"), verbose=False
        )
        budgets: list[tuple[str, float | None]] = []
        elapsed = 0.0

        def container(
            _stage: output.Stage,
            _workspace: Path,
            _command: list[str],
            *,
            timeout: float,
            **_options: object,
        ) -> None:
            nonlocal elapsed
            budgets.append(("container", timeout))
            elapsed += 110.0

        def host(_command: list[str], *, timeout: float, **_kwargs: object) -> None:
            budgets.append(("host", timeout))

        with (
            mock.patch.object(
                host_testing,
                "host_pytest_environment",
                return_value=contextlib.nullcontext((Path(sys.executable), {})),
            ),
            # The external subprocess boundaries consume a controlled amount of the clock.
            mock.patch.object(runtime, "run_quality_command", side_effect=container),
            mock.patch.object(output.Stage, "run", side_effect=host),
            mock.patch.object(time, "monotonic", side_effect=lambda: elapsed),
        ):
            testing.run_test_workload(
                reporter,
                tmp_path,
                kern="kern",
                rootfs=Path("/environment/rootfs"),
                names=[],
                tier="host_process",
            )
        assert budgets == [("container", 300.0), ("host", 190.0)]

    def test_failing_host_tests_keep_earned_python_success_without_tests_success(
        self, tmp_path: Path
    ) -> None:
        """Host test failure preserves successful analyzers and the previous test receipt."""
        module = tmp_path / "workspace/tests/host_process/cache/test_prune_rootfs_ownership.py"
        module.parent.mkdir(parents=True)
        module.touch()
        cache = tmp_path / ".cache"
        original = CheckReceiptRecipe("tests", "a" * 64, "b" * 64, "c" * 64)
        changed = replace(original, closure_digest="d" * 64)
        python = replace(changed, scope="python")
        publish_success_receipt(cache, original)
        reporter = output.RunReporter(
            "check", tmp_path / "logs", str(tmp_path / "logs"), verbose=False
        )
        with (
            mock.patch.object(
                host_testing,
                "host_pytest_environment",
                return_value=contextlib.nullcontext((Path(sys.executable), {})),
            ),
            # Source tools and Kern are external to receipt publication; only host pytest fails.
            mock.patch.object(checks, "run_quality_command"),
            mock.patch.object(runtime, "run_quality_command"),
            mock.patch.object(output.Stage, "run", side_effect=SystemExit(1)),
            pytest.raises(SystemExit) as raised,
        ):
            checks._run_missing_checks(  # noqa: SLF001 -- exercise the receipt publishing owner.
                reporter=reporter,
                cache=cache,
                missing=("python", "tests"),
                analyzer_cache={},
                workspace=tmp_path / "workspace",
                kern="kern",
                rootfs=Path("/environment/rootfs"),
                recipes={"python": python, "tests": changed},
                profile=None,
                build_type="release",
                jobs=1,
            )
        assert raised.value.code == 1
        assert receipt_matches(cache, original)
        assert not receipt_matches(cache, changed)
        assert receipt_matches(cache, python)

    @pytest.mark.parametrize(
        "implementation",
        [
            "scripts/fplinux_cli/quality/host_testing.py",
            "scripts/fplinux_cli/dependencies/inputs.py",
            "scripts/fplinux_cli/quality/scopes.py",
        ],
        ids=["host-environment", "dependency-declarations", "check-selection"],
    )
    def test_preparation_and_selection_edits_revoke_success_receipts(
        self, tmp_path: Path, implementation: str
    ) -> None:
        """Dependency preparation and check selection changes invalidate cached checks."""
        shutil.copytree(
            ROOT / "scripts/fplinux_cli",
            tmp_path / "scripts/fplinux_cli",
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        cache = tmp_path / ".cache"
        with mock.patch.object(common, "ROOT", tmp_path):
            original = CheckReceiptRecipe(
                "tests", "a" * 64, check_orchestration_recipe_digest("b" * 64), "c" * 64
            )
            publish_success_receipt(cache, original)
            assert receipt_matches(cache, original)
            unrelated = tmp_path / "notes.txt"
            unrelated.write_text("unrelated local note\n")
            same = replace(
                original, orchestration_recipe=check_orchestration_recipe_digest("b" * 64)
            )
            assert receipt_matches(cache, same)
            source = tmp_path / implementation
            source.write_text(source.read_text() + "\n# changed check inputs\n")
            changed = replace(
                original, orchestration_recipe=check_orchestration_recipe_digest("b" * 64)
            )
            assert not receipt_matches(cache, changed)

    def test_host_python_change_revokes_tests_success_without_invalidating_analyzers(
        self, tmp_path: Path
    ) -> None:
        """Host runtime identity belongs to tests; Python and C analyzers use the environment."""

        def recipe(scope: str) -> CheckReceiptRecipe:
            return check_scope_receipt_recipe(
                scope,
                "a" * 64,
                environment_generation="b" * 64,
                orchestration_recipe="c" * 64,
            )

        for scope in ("python", "tests", "c"):
            publish_success_receipt(tmp_path, recipe(scope))
            assert receipt_matches(tmp_path, recipe(scope))
        with mock.patch.object(sys, "version", "3.14.8 (a different host runtime)"):
            assert not receipt_matches(tmp_path, recipe("tests"))
            assert receipt_matches(tmp_path, recipe("python"))
            assert receipt_matches(tmp_path, recipe("c"))
        with mock.patch.object(sys.implementation, "cache_tag", "cpython-314-other"):
            assert not receipt_matches(tmp_path, recipe("tests"))
            assert receipt_matches(tmp_path, recipe("python"))
            assert receipt_matches(tmp_path, recipe("c"))

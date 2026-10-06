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
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.quality import checks, testing
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
from fplinux_cli.workspace import staging as workspace_staging

from tests import ROOT


class SelectedTestRunTests:
    """A test result owns logs, never a full quality-check success receipt."""

    def test_named_selection_counts_each_tiers_time_budget_once(self) -> None:
        """Names spanning tiers get their combined budget; more names in a tier add nothing."""

        def selected_budget(*names: str) -> int:
            [(_label, _command, timeout)] = testing.pytest_commands(list(names))
            return timeout

        small = selected_budget("tests.small.environment.test_common.FirstCase")
        host_process = selected_budget("tests.host_process.test_process")
        assert small == 90
        assert host_process == 180
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
            == 270
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
            recipe = CheckReceiptRecipe("python", "a" * 64, "b" * 64, "c" * 64)
            publish_success_receipt(cache, recipe)
            receipt = receipt_path(cache, recipe)
            original = receipt.read_bytes()
            fixture = root / "tests/small/test_example.py"
            fixture.parent.mkdir(parents=True)
            fixture.write_text("# fixture\n")
            with mock.patch.object(common, "ROOT", root):
                snapshot = workspace.workspace_snapshot([("tests/small/test_example.py", fixture)])
            with (
                mock.patch.object(workspace_staging, "ROOT", root),
                mock.patch.object(output, "ROOT", root),
                mock.patch.object(testing, "quality_workspace_snapshot", return_value=snapshot),
                mock.patch.object(testing, "load_container_lock", return_value={}),
                mock.patch.object(testing, "container_image_recipe_digest", return_value="b" * 64),
                mock.patch.object(
                    testing,
                    "prepare_quality_image",
                    return_value=(
                        "kern",
                        "locked-image",
                        ImageState("b" * 64, "c" * 64, "c" * 64),
                    ),
                ),
                # Kern itself is outside this unit boundary; selection has real process tests.
                mock.patch.object(
                    testing,
                    "run_quality_command",
                    side_effect=failure,
                ),
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
            assert (receipt.read_bytes()) == (original)
            assert (list((cache / "check-results").rglob("*.json"))) == ([receipt])
            assert (list((cache / "quality-workspaces").iterdir())) == ([])
            runs = list((cache / "logs/test").glob("*/run.json"))
            assert (len(runs)) == (1)
            assert (json.loads(runs[0].read_text())["status"]) == (status)

    def test_host_and_container_use_one_host_process_deadline(self, tmp_path: Path) -> None:
        """A host subprocess receives only the time left by the preceding container work."""
        module = tmp_path / "tests/host_process/cache/test_image_tag.py"
        module.parent.mkdir(parents=True)
        module.touch()
        reporter = output.RunReporter(
            "test", tmp_path / "logs", str(tmp_path / "logs"), verbose=False
        )
        budgets: list[tuple[str, float | None]] = []

        def container(
            _stage: output.Stage,
            _workspace: Path,
            _command: list[str],
            *,
            kern: str,
            image: str,
            timeout: float,
            log_name: str | None,
        ) -> None:
            del kern, image, log_name
            budgets.append(("container", timeout))

        def host(_command: list[str], *, timeout: float, **_kwargs: object) -> None:
            budgets.append(("host", timeout))

        with (
            mock.patch.object(
                testing,
                "host_pytest_environment",
                return_value=contextlib.nullcontext((Path(sys.executable), {})),
            ),
            # The external subprocess boundaries consume a controlled amount of the clock.
            mock.patch.object(testing, "run_quality_command", side_effect=container),
            mock.patch.object(output.Stage, "run", side_effect=host),
            mock.patch.object(time, "monotonic", side_effect=[0.0, 0.0, 110.0]),
        ):
            testing.run_test_workload(
                reporter, tmp_path, kern="kern", image="image", names=[], tier="host_process"
            )
        assert budgets == [("container", 180.0), ("host", 70.0)]

    def test_failing_host_tests_do_not_publish_python_success(self, tmp_path: Path) -> None:
        """A successful source check and image workload cannot certify a failed host case."""
        module = tmp_path / "workspace/tests/host_process/cache/test_image_tag.py"
        module.parent.mkdir(parents=True)
        module.touch()
        cache = tmp_path / ".cache"
        original = CheckReceiptRecipe("python", "a" * 64, "b" * 64, "c" * 64)
        changed = replace(original, closure_digest="d" * 64)
        publish_success_receipt(cache, original)
        reporter = output.RunReporter(
            "check", tmp_path / "logs", str(tmp_path / "logs"), verbose=False
        )
        with (
            mock.patch.object(
                testing,
                "host_pytest_environment",
                return_value=contextlib.nullcontext((Path(sys.executable), {})),
            ),
            # Source tools and Kern are external to receipt publication; only host pytest fails.
            mock.patch.object(checks, "run_quality_command"),
            mock.patch.object(testing, "run_quality_command"),
            mock.patch.object(output.Stage, "run", side_effect=SystemExit(1)),
            pytest.raises(SystemExit) as raised,
        ):
            checks._run_missing_checks(  # noqa: SLF001 -- exercise the receipt publishing owner.
                reporter=reporter,
                cache=cache,
                missing=("python",),
                analyzer_cache={},
                workspace=tmp_path / "workspace",
                kern="kern",
                image="image",
                recipes={"python": changed},
                profile=None,
                build_type="release",
                jobs=1,
            )
        assert raised.value.code == 1
        assert receipt_matches(cache, original)
        assert not receipt_matches(cache, changed)

    @pytest.mark.parametrize(
        "implementation",
        [
            "scripts/fplinux_cli/quality/host_testing.py",
            "scripts/fplinux_cli/dependencies/inputs.py",
        ],
        ids=["host-environment", "dependency-declarations"],
    )
    def test_host_preparation_edits_revoke_success_receipts(
        self, tmp_path: Path, implementation: str
    ) -> None:
        """Dependency selection and host environment changes must invalidate cached checks."""
        shutil.copytree(
            ROOT / "scripts/fplinux_cli",
            tmp_path / "scripts/fplinux_cli",
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
        )
        cache = tmp_path / ".cache"
        with mock.patch.object(common, "ROOT", tmp_path):
            original = CheckReceiptRecipe(
                "python", "a" * 64, check_orchestration_recipe_digest("b" * 64), "c" * 64
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
            source.write_text(source.read_text() + "\n# changed preparation\n")
            changed = replace(
                original, orchestration_recipe=check_orchestration_recipe_digest("b" * 64)
            )
            assert not receipt_matches(cache, changed)

    def test_host_python_change_revokes_only_python_success(self, tmp_path: Path) -> None:
        """The host runtime affects Python test receipts without changing C check inputs."""

        def recipe(scope: str) -> CheckReceiptRecipe:
            return check_scope_receipt_recipe(
                scope,
                "a" * 64,
                image_generation="b" * 64,
                orchestration_recipe="c" * 64,
            )

        python = recipe("python")
        c = recipe("c")
        publish_success_receipt(tmp_path, python)
        publish_success_receipt(tmp_path, c)
        assert receipt_matches(tmp_path, recipe("python"))
        with mock.patch.object(sys, "version", "3.14.8 (a different host runtime)"):
            assert not receipt_matches(tmp_path, recipe("python"))
            assert receipt_matches(tmp_path, recipe("c"))

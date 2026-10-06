# SPDX-License-Identifier: GPL-2.0-only
"""Selected-test orchestration with a stubbed external Kern process boundary."""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.environment.image_state import ImageState
from fplinux_cli.quality import testing
from fplinux_cli.quality.receipts import CheckReceiptRecipe, publish_success_receipt, receipt_path
from fplinux_cli.reporting import run as output
from fplinux_cli.workspace import capture as workspace
from fplinux_cli.workspace import staging as workspace_staging


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

# SPDX-License-Identifier: GPL-2.0-only
"""Selected-test orchestration with a stubbed external Kern process boundary."""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import output, workspace
from fplinux_cli.checkreceipts import CheckReceiptRecipe, publish_success_receipt, receipt_path
from fplinux_cli.image_state import ImageState
from fplinux_cli.quality import testing


class SelectedTestRunTests(unittest.TestCase):
    """A test result owns logs, never a full quality-check success receipt."""

    def test_discovery_keeps_each_tiers_time_budget(self) -> None:
        """Short checks and artifact work retain their distinct documented deadlines."""
        self.assertEqual(
            [(label, timeout) for label, _command, timeout in testing.unittest_commands([])],
            [
                ("small", 90),
                ("host_process", 180),
                ("host_tool", 240),
                ("artifact", 300),
                ("public_workflow", 90),
            ],
        )

    def test_named_selection_counts_each_tiers_time_budget_once(self) -> None:
        """Selecting more names in one group must not multiply its available runtime."""
        commands = testing.unittest_commands(
            [
                "tests.small.test_common.FirstCase",
                "tests.small.test_common.SecondCase",
                "tests.host_process.test_process",
            ]
        )
        self.assertEqual([timeout for _label, _command, timeout in commands], [270])

    def test_all_outcomes_preserve_check_receipts_and_release_workspace(self) -> None:
        """Success, failure and interruption preserve check evidence and release staged sources."""
        for status, failure, exit_code in (
            ("success", None, 0),
            ("failed", SystemExit(1), 1),
            ("interrupted", KeyboardInterrupt(), 130),
        ):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                cache = root / ".cache"
                recipe = CheckReceiptRecipe("python", "a" * 64, "b" * 64, "c" * 64)
                publish_success_receipt(cache, recipe)
                receipt = receipt_path(cache, recipe)
                original = receipt.read_bytes()
                fixture = root / "tests/small/test_example.py"
                fixture.parent.mkdir(parents=True)
                fixture.write_text("# fixture\n")
                with mock.patch.object(workspace, "ROOT", root):
                    snapshot = workspace.workspace_snapshot(
                        [("tests/small/test_example.py", fixture)]
                    )
                with (
                    mock.patch.object(workspace, "ROOT", root),
                    mock.patch.object(output, "ROOT", root),
                    mock.patch.object(
                        testing, "quality_workspace_snapshot", return_value=snapshot
                    ),
                    mock.patch.object(testing, "load_container_lock", return_value={}),
                    mock.patch.object(
                        testing, "container_image_recipe_digest", return_value="b" * 64
                    ),
                    mock.patch.object(
                        testing,
                        "prepare_quality_image",
                        return_value=("kern", "locked-image", ImageState("b" * 64, "c" * 64)),
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
                        with self.assertRaises(SystemExit) as raised:
                            output.run_entrypoint(
                                lambda: testing.run_tests(["tests.small.test_example"])
                            )
                        self.assertEqual(raised.exception.code, exit_code)
                    else:
                        output.run_entrypoint(
                            lambda: testing.run_tests(["tests.small.test_example"])
                        )
                self.assertEqual(receipt.read_bytes(), original)
                self.assertEqual(list((cache / "check-results").rglob("*.json")), [receipt])
                self.assertEqual(list((cache / "quality-workspaces").iterdir()), [])
                runs = list((cache / "logs/test").glob("*/run.json"))
                self.assertEqual(len(runs), 1)
                self.assertEqual(json.loads(runs[0].read_text())["status"], status)


if __name__ == "__main__":
    unittest.main()

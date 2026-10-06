# SPDX-License-Identifier: GPL-2.0-only
"""Exercise selection and result propagation through real pytest subprocesses."""

from __future__ import annotations

import json
import os
import shutil
import signal
import sys
import tempfile
import time
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import python_environment, run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable


class TestSelectionProcessTests:
    """Run the in-image entry point on controlled files; this is not a Kern test."""

    @pytest.fixture(autouse=True)
    def _selection_workspace(self, tmp_path: Path) -> None:
        """Keep the fixture package and its outcomes independent of the real suite."""
        self.root = tmp_path
        self.trace = self.root / "executed.txt"
        shutil.copyfile(ROOT / "pyproject.toml", self.root / "pyproject.toml")
        (self.root / "tests").mkdir()
        (self.root / "tests/__init__.py").touch()
        for tier in ("small", "host_process", "host_tool", "artifact", "public_workflow"):
            directory = self.root / "tests" / tier
            directory.mkdir()
            (directory / "__init__.py").touch()
            nested = directory / "domain"
            nested.mkdir()
            (nested / "__init__.py").touch()
            for package in (directory, nested):
                shutil.copyfile(
                    ROOT / "tests/fixtures/processes/selection_cases.py",
                    package / "test_selection.py",
                )

    def run_selection(
        self,
        *arguments: str,
        failing_fixture: bool = True,
        while_running: Callable[[subprocess.Popen[str], float], None] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Execute the production entry point with controlled process boundaries."""
        self.logs = Path(tempfile.mkdtemp(dir=self.root, prefix="logs-"))
        return run_process(
            [sys.executable, "-m", "fplinux_cli.quality.testing", *arguments],
            name="selected pytest process",
            cwd=self.root,
            env={
                **python_environment(),
                "FPLINUX_TEST_TRACE": str(self.trace),
                "FPLINUX_TEST_FAIL": "1" if failing_fixture else "0",
                "FPLINUX_LOG_ROOT": str(self.logs),
                "FPLINUX_LOG_DISPLAY_ROOT": str(self.logs),
                "FPLINUX_VERBOSE": "1" if "--verbose" in arguments else "0",
            },
            timeout=15,
            while_running=while_running,
        )

    def executed(self) -> list[str]:
        """Read the independent fixture's observed execution trace."""
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def test_class_and_method_select_only_requested_cases(self) -> None:
        """A class runs its two tests; a method does not run its sibling."""
        prefix = "tests.small.test_selection.PassingCase"
        result = self.run_selection(prefix)
        assert result.returncode == 0, result.stderr
        assert self.executed() == [prefix + ".test_first", prefix + ".test_second"]
        self.trace.unlink()
        result = self.run_selection(prefix + ".test_second", "--verbose")
        assert result.returncode == 0, result.stderr
        assert self.executed() == [prefix + ".test_second"]
        assert "test_second" in (self.logs / "tests/01-selected.log").read_text()
        assert "test_second" in result.stdout + result.stderr

    def test_no_selection_runs_every_tier_in_order(self) -> None:
        """Default discovery must execute all five groups, not just the first group."""
        result = self.run_selection(failing_fixture=False)
        assert result.returncode == 0, result.stderr
        assert self.executed() == [
            f"tests.{tier}.{package}test_selection.{case}.{method}"
            for tier in ("small", "host_process", "host_tool", "artifact", "public_workflow")
            for package in ("domain.", "")
            for case in ("FailureCase", "PassingCase")
            for method in ("test_first", "test_second")
        ]

    @pytest.mark.parametrize(
        ("suffix", "cases"),
        [
            (
                "",
                [
                    "FailureCase.test_first",
                    "FailureCase.test_second",
                    "PassingCase.test_first",
                    "PassingCase.test_second",
                ],
            ),
            (".PassingCase", ["PassingCase.test_first", "PassingCase.test_second"]),
            (".PassingCase.test_second", ["PassingCase.test_second"]),
        ],
    )
    def test_nested_module_class_and_method_select_only_requested_cases(
        self, suffix: str, cases: list[str]
    ) -> None:
        """Nested selectors preserve module, class and method boundaries."""
        module = "tests.small.domain.test_selection"
        result = self.run_selection(module + suffix, failing_fixture=False)
        assert result.returncode == 0, result.stderr
        assert self.executed() == [module + "." + case for case in cases]

    def test_multiple_names_preserve_each_selection(self) -> None:
        """Names from different tiers run together without discovering neighboring tests."""
        names = (
            "tests.small.test_selection.PassingCase.test_second",
            "tests.host_tool.test_selection.PassingCase.test_first",
        )
        result = self.run_selection(*names)
        assert result.returncode == 0, result.stderr
        assert self.executed() == list(names)

    @pytest.mark.parametrize(
        ("suffixes", "expected"),
        [
            (("test_first", "test_first"), ["test_first", "test_first"]),
            (("test_second", ""), ["test_second", "test_first", "test_second"]),
            (("", "test_first"), ["test_first", "test_second", "test_first"]),
        ],
    )
    def test_repeated_and_overlapping_names_preserve_requested_order(
        self, suffixes: tuple[str, ...], expected: list[str]
    ) -> None:
        """Selecting the same case again retains both its position and execution."""
        prefix = "tests.small.test_selection.PassingCase"
        names = [prefix + ("." + suffix if suffix else "") for suffix in suffixes]
        result = self.run_selection(*names)
        assert result.returncode == 0, result.stderr
        assert self.executed() == [prefix + "." + method for method in expected]

    @pytest.mark.parametrize("failfast", [False, True])
    def test_unresolved_name_does_not_cancel_valid_selection_without_failfast(
        self, *, failfast: bool
    ) -> None:
        """A bad name fails the run and only failfast prevents the following valid case."""
        valid = "tests.small.test_selection.PassingCase.test_first"
        arguments = [
            "tests.small.test_selection.MissingCase",
            valid,
            *(["--failfast"] if failfast else []),
        ]
        result = self.run_selection(*arguments)
        assert result.returncode == 1, result.stderr
        assert self.executed() == ([] if failfast else [valid])

    @pytest.mark.parametrize("empty_first", [False, True])
    def test_empty_name_does_not_turn_a_nonempty_selection_into_failure(
        self, *, empty_first: bool
    ) -> None:
        """An empty class does not change a successful selected case's outcome."""
        empty = "tests.small.test_selection.EmptyCase"
        valid = "tests.small.test_selection.PassingCase.test_first"
        names = (empty, valid) if empty_first else (valid, empty)
        result = self.run_selection(*names)
        assert result.returncode == 0, result.stderr
        assert self.executed() == [valid]

    def test_ambient_pytest_options_and_plugins_do_not_change_execution(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Ambient collection flags and plugins cannot replace the pinned workload."""
        monkeypatch.setenv("PYTEST_ADDOPTS", "--collect-only")
        monkeypatch.setenv("PYTEST_PLUGINS", "fplinux_missing_plugin")
        name = "tests.small.test_selection.PassingCase.test_second"
        result = self.run_selection(name)
        assert result.returncode == 0, result.stderr
        assert self.executed() == [name]

    @pytest.mark.parametrize("tier_selection", [False, True])
    def test_module_and_tier_discovery_propagate_failures(self, *, tier_selection: bool) -> None:
        """Both selection forms run the chosen module and not another tier."""
        prefix = "tests.small.test_selection"
        expected = [
            prefix + ".FailureCase.test_first",
            prefix + ".FailureCase.test_second",
            prefix + ".PassingCase.test_first",
            prefix + ".PassingCase.test_second",
        ]
        arguments = ("--tier", "small") if tier_selection else (prefix,)
        nested = "tests.small.domain.test_selection"
        selected = [name.replace(prefix, nested) for name in expected] + expected
        result = self.run_selection(*arguments)
        assert result.returncode == 1, result.stderr
        assert self.executed() == (selected if tier_selection else expected)
        assert "intentional selection-fixture failure" in result.stderr

    def test_failfast_stops_at_the_first_failure(self) -> None:
        """Failfast stops execution rather than merely accepting an unused flag."""
        prefix = "tests.small.test_selection.FailureCase"
        result = self.run_selection(prefix, "--failfast")
        assert result.returncode == 1, result.stderr
        assert self.executed() == [prefix + ".test_first"]

    @pytest.mark.parametrize(
        ("selection", "status"),
        [
            ("tests.small.test_selection.MissingCase", 1),
            ("tests.small.test_selection.EmptyCase", 5),
            ("tests.small.domain", 5),
        ],
    )
    def test_missing_or_empty_selection_is_not_success(self, selection: str, status: int) -> None:
        """Unknown names fail and empty classes or packages keep the no-tests status."""
        result = self.run_selection(selection)
        assert result.returncode == status, result.stderr
        assert self.executed() == []

    def test_native_node_selects_one_case(self) -> None:
        """A pytest node ID selects its method without running siblings."""
        result = self.run_selection("tests/small/test_selection.py::PassingCase::test_second")
        assert result.returncode == 0, result.stderr
        assert self.executed() == ["tests.small.test_selection.PassingCase.test_second"]

    def test_native_parameter_node_selects_one_input(self) -> None:
        """A parameter ID selects only the named input of a native pytest function."""
        fixture = self.root / "tests/small/test_parameters.py"
        fixture.write_text(
            "import os\n"
            "from pathlib import Path\n"
            "import pytest\n"
            "@pytest.mark.parametrize('value', ['first', 'second'])\n"
            "def test_value(value):\n"
            "    with Path(os.environ['FPLINUX_TEST_TRACE']).open('a') as stream:\n"
            "        stream.write(value + '\\n')\n"
        )
        result = self.run_selection("tests/small/test_parameters.py::test_value[second]")
        assert result.returncode == 0, result.stderr
        assert self.executed() == ["second"]

    @pytest.mark.parametrize("failfast", [False, True])
    def test_import_failure_returns_failure_and_obeys_failfast(self, *, failfast: bool) -> None:
        """An import error fails the tier, and failfast prevents further test execution."""
        fixture = self.root / "tests/small/test_broken.py"
        fixture.write_text("raise RuntimeError('intentional collection-fixture failure')\n")
        arguments = ["--tier", "small", *(["--failfast"] if failfast else [])]
        result = self.run_selection(*arguments, failing_fixture=False)
        assert result.returncode == 1, result.stderr
        assert "intentional collection-fixture failure" in result.stderr
        expected = [
            f"tests.small.{package}test_selection.{case}.{method}"
            for package in ("domain.", "")
            for case in ("FailureCase", "PassingCase")
            for method in ("test_first", "test_second")
        ]
        assert self.executed() == ([] if failfast else expected)

    def test_child_sigint_returns_shell_interruption_status(self) -> None:
        """A SIGINT in pytest itself retains interruption rather than failure status."""
        fixture = self.root / "tests/small/test_interrupt.py"
        fixture.write_text(
            "import os\n"
            "import signal\n"
            "def test_interrupt():\n"
            "    os.kill(os.getpid(), signal.SIGINT)\n"
        )
        result = self.run_selection("tests.small.test_interrupt")
        assert result.returncode == 130, result.stderr
        assert json.loads((self.logs / "tests/run.json").read_text())["status"] == "interrupted"

    def test_sigint_returns_shell_interruption_status(self) -> None:
        """SIGINT reaches a running pytest process and returns 130 after cleanup."""
        ready = self.root / "ready"
        fixture = self.root / "tests/small/test_wait.py"
        fixture.write_text(
            "import time\n"
            "from pathlib import Path\n"
            "def test_wait():\n"
            "    Path('ready').touch()\n"
            "    time.sleep(30)\n"
        )

        def interrupt_when_ready(process: subprocess.Popen[str], deadline: float) -> None:
            while not ready.exists():
                assert process.poll() is None, "runner exited before the waiting case"
                assert time.monotonic() < deadline, "waiting case never started"
                time.sleep(0.01)
            os.kill(process.pid, signal.SIGINT)

        result = self.run_selection("tests.small.test_wait", while_running=interrupt_when_ready)
        assert result.returncode == 130, result.stderr
        assert json.loads((self.logs / "tests/run.json").read_text())["status"] == "interrupted"

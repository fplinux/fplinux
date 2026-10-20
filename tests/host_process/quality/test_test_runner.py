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
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.process import python_environment, run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable


@dataclass
class SelectionContext:
    """Own a controlled test tree and its observed pytest execution."""

    root: Path
    trace: Path
    logs: Path = field(init=False)

    def run(
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


@pytest.fixture
def selection_context(tmp_path: Path) -> SelectionContext:
    """Keep the fixture package and its outcomes independent of the real suite."""
    shutil.copyfile(ROOT / "pyproject.toml", tmp_path / "pyproject.toml")
    (tmp_path / "tests").mkdir()
    (tmp_path / "tests/__init__.py").touch()
    for tier in ("small", "host_process", "host_tool", "artifact", "public_workflow"):
        directory = tmp_path / "tests" / tier
        directory.mkdir()
        (directory / "__init__.py").touch()
        for package in (directory, directory / "build", directory / "domain"):
            package.mkdir(exist_ok=True)
            (package / "__init__.py").touch()
            shutil.copyfile(
                ROOT / "tests/host_process/quality/fixtures/test-runner/selection_cases.py",
                package / "test_selection.py",
            )
    return SelectionContext(tmp_path, tmp_path / "executed.txt")


def _prepare_gui_selection_modules(root: Path) -> None:
    """Give a GUI component and its shared-language neighbor observable tests."""
    directory = root / "tests/host_tool/gui"
    directory.mkdir(parents=True, exist_ok=True)
    (directory.parent / "__init__.py").touch()
    (directory / "__init__.py").touch()
    for module in ("test_desktop_entry.py", "test_language_files.py"):
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/selection_cases.py",
            directory / module,
        )


class TestSelectionProcessTests:
    """Run the in-image entry point on controlled files; this is not a Kern test."""

    @pytest.mark.parametrize("selection", ["tests/small/domain", "tests/small/domain/"])
    def test_directory_discovers_only_its_tests(
        self, selection_context: SelectionContext, selection: str
    ) -> None:
        """A selected directory uses normal pytest discovery without neighboring packages."""
        result = selection_context.run(selection, failing_fixture=False)

        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [
            "tests.small.domain.test_selection.FailureCase.test_first",
            "tests.small.domain.test_selection.FailureCase.test_second",
            "tests.small.domain.test_selection.PassingCase.test_first",
            "tests.small.domain.test_selection.PassingCase.test_second",
        ]

    def test_class_and_method_select_only_requested_cases(
        self, selection_context: SelectionContext
    ) -> None:
        """A class runs its two tests; a method does not run its sibling."""
        prefix = "tests.small.test_selection.PassingCase"
        result = selection_context.run(prefix)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [prefix + ".test_first", prefix + ".test_second"]
        selection_context.trace.unlink()
        result = selection_context.run(prefix + ".test_second", "--verbose")
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [prefix + ".test_second"]
        assert "test_second" in (selection_context.logs / "tests/01-selected.log").read_text()
        assert "test_second" in result.stdout + result.stderr

    def test_no_selection_runs_every_tier_in_order(
        self, selection_context: SelectionContext
    ) -> None:
        """Default discovery must execute all five groups, not just the first group."""
        result = selection_context.run(failing_fixture=False)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [
            f"tests.{tier}.{package}test_selection.{case}.{method}"
            for tier in ("small", "host_process", "host_tool", "artifact", "public_workflow")
            for package in ("build.", "domain.", "")
            for case in ("FailureCase", "PassingCase")
            for method in ("test_first", "test_second")
        ]

    @pytest.mark.parametrize("select_tier", [False, True])
    def test_automatic_discovery_runs_gui_and_its_shared_neighbor(
        self, selection_context: SelectionContext, *, select_tier: bool
    ) -> None:
        """Both automatic selection forms execute GUI components and shared tests."""
        _prepare_gui_selection_modules(selection_context.root)

        result = selection_context.run(
            *(["--tier", "host_tool"] if select_tier else []), failing_fixture=False
        )

        assert result.returncode == 0, result.stderr
        assert [name for name in selection_context.executed() if ".gui." in name] == [
            "tests.host_tool.gui.test_desktop_entry.FailureCase.test_first",
            "tests.host_tool.gui.test_desktop_entry.FailureCase.test_second",
            "tests.host_tool.gui.test_desktop_entry.PassingCase.test_first",
            "tests.host_tool.gui.test_desktop_entry.PassingCase.test_second",
            "tests.host_tool.gui.test_language_files.FailureCase.test_first",
            "tests.host_tool.gui.test_language_files.FailureCase.test_second",
            "tests.host_tool.gui.test_language_files.PassingCase.test_first",
            "tests.host_tool.gui.test_language_files.PassingCase.test_second",
        ]
        assert (
            "tests.host_tool.test_selection.PassingCase.test_first" in selection_context.executed()
        )

    @pytest.mark.parametrize(
        "selection",
        [
            "tests.host_tool.gui.test_desktop_entry",
            "tests/host_tool/gui/test_desktop_entry.py",
        ],
        ids=["dotted", "native"],
    )
    def test_explicit_gui_selection_keeps_ordinary_collection_failure(
        self, selection_context: SelectionContext, selection: str
    ) -> None:
        """An explicit GUI module reports its real pytest collection error."""
        _prepare_gui_selection_modules(selection_context.root)
        (selection_context.root / "tests/host_tool/gui/test_desktop_entry.py").write_text(
            "raise RuntimeError('intentional GUI collection failure')\n"
        )

        result = selection_context.run(selection, failing_fixture=False)

        assert result.returncode == 1, result.stderr
        assert "intentional GUI collection failure" in result.stdout + result.stderr
        assert selection_context.executed() == []

    @pytest.mark.parametrize("package", ["build", "domain"])
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
        self, selection_context: SelectionContext, package: str, suffix: str, cases: list[str]
    ) -> None:
        """Nested selectors preserve module, class and method boundaries."""
        module = f"tests.small.{package}.test_selection"
        result = selection_context.run(module + suffix, failing_fixture=False)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [module + "." + case for case in cases]

    def test_multiple_names_preserve_each_selection(
        self, selection_context: SelectionContext
    ) -> None:
        """Names from different tiers run together without discovering neighboring tests."""
        names = (
            "tests.small.test_selection.PassingCase.test_second",
            "tests.host_tool.test_selection.PassingCase.test_first",
        )
        result = selection_context.run(*names)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == list(names)

    @pytest.mark.parametrize(
        ("suffixes", "expected"),
        [
            (("test_first", "test_first"), ["test_first", "test_first"]),
            (("test_second", ""), ["test_second", "test_first", "test_second"]),
            (("", "test_first"), ["test_first", "test_second", "test_first"]),
        ],
    )
    def test_repeated_and_overlapping_names_preserve_requested_order(
        self, selection_context: SelectionContext, suffixes: tuple[str, ...], expected: list[str]
    ) -> None:
        """Selecting the same case again retains both its position and execution."""
        prefix = "tests.small.test_selection.PassingCase"
        names = [prefix + ("." + suffix if suffix else "") for suffix in suffixes]
        result = selection_context.run(*names)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [prefix + "." + method for method in expected]

    @pytest.mark.parametrize("failfast", [False, True])
    def test_unresolved_name_does_not_cancel_valid_selection_without_failfast(
        self, selection_context: SelectionContext, *, failfast: bool
    ) -> None:
        """A bad name fails the run and only failfast prevents the following valid case."""
        valid = "tests.small.test_selection.PassingCase.test_first"
        arguments = [
            "tests.small.test_selection.MissingCase",
            valid,
            *(["--failfast"] if failfast else []),
        ]
        result = selection_context.run(*arguments)
        assert result.returncode == 1, result.stderr
        assert selection_context.executed() == ([] if failfast else [valid])

    @pytest.mark.parametrize("empty_first", [False, True])
    def test_empty_name_does_not_turn_a_nonempty_selection_into_failure(
        self, selection_context: SelectionContext, *, empty_first: bool
    ) -> None:
        """An empty class does not change a successful selected case's outcome."""
        empty = "tests.small.test_selection.EmptyCase"
        valid = "tests.small.test_selection.PassingCase.test_first"
        names = (empty, valid) if empty_first else (valid, empty)
        result = selection_context.run(*names)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [valid]

    def test_ambient_pytest_options_and_plugins_do_not_change_execution(
        self, selection_context: SelectionContext, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """Ambient collection flags and plugins cannot replace the pinned workload."""
        monkeypatch.setenv("PYTEST_ADDOPTS", "--collect-only")
        monkeypatch.setenv("PYTEST_PLUGINS", "fplinux_missing_plugin")
        name = "tests.small.test_selection.PassingCase.test_second"
        result = selection_context.run(name)
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [name]

    def test_unknown_marker_fails_before_executing_the_case(
        self, selection_context: SelectionContext
    ) -> None:
        """A misspelled marker must not silently run as an unmarked test."""
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_typo_marker.py",
            selection_context.root / "tests/small/test_marker.py",
        )

        result = selection_context.run("tests.small.test_marker")

        assert result.returncode == 1
        assert "'fplinux_typo_marker' not found in `markers`" in result.stdout + result.stderr
        assert selection_context.executed() == []

    def test_unknown_configuration_fails_before_executing_the_case(
        self, selection_context: SelectionContext
    ) -> None:
        """Configuration typos fail rather than leaving the intended setting inactive."""
        configuration = selection_context.root / "pyproject.toml"
        contents = configuration.read_text()
        header = "[tool.pytest.ini_options]"
        assert contents.count(header) == 1
        configuration.write_text(contents.replace(header, header + "\nfplinux_typo_option = true"))

        result = selection_context.run("tests.small.test_selection.PassingCase.test_first")

        assert result.returncode == 1
        assert "Unknown config option: fplinux_typo_option" in result.stdout + result.stderr
        assert selection_context.executed() == []

    def test_skip_reason_and_duration_summary_are_reported(
        self, selection_context: SelectionContext
    ) -> None:
        """Quiet runs retain skip reasons and timings in their owned pytest log."""
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_diagnostic_cases.py",
            selection_context.root / "tests/small/test_diagnostics.py",
        )

        result = selection_context.run("tests.small.test_diagnostics")

        assert result.returncode == 0, result.stderr
        report = (selection_context.logs / "tests/01-selected.log").read_text()
        assert "SKIPPED" in report
        assert "controlled skip explanation" in report
        assert "slowest 20 durations" in report

    @pytest.mark.parametrize("tier_selection", [False, True])
    def test_module_and_tier_discovery_propagate_failures(
        self, selection_context: SelectionContext, *, tier_selection: bool
    ) -> None:
        """Both selection forms run the chosen module and not another tier."""
        prefix = "tests.small.test_selection"
        expected = [
            prefix + ".FailureCase.test_first",
            prefix + ".FailureCase.test_second",
            prefix + ".PassingCase.test_first",
            prefix + ".PassingCase.test_second",
        ]
        arguments = ("--tier", "small") if tier_selection else (prefix,)
        selected = [
            name.replace(prefix, f"tests.small.{package}test_selection")
            for package in ("build.", "domain.", "")
            for name in expected
        ]
        result = selection_context.run(*arguments)
        assert result.returncode == 1, result.stderr
        assert selection_context.executed() == (selected if tier_selection else expected)
        assert "intentional selection-fixture failure" in result.stderr

    def test_failfast_stops_at_the_first_failure(
        self, selection_context: SelectionContext
    ) -> None:
        """Failfast stops execution rather than merely accepting an unused flag."""
        prefix = "tests.small.test_selection.FailureCase"
        result = selection_context.run(prefix, "--failfast")
        assert result.returncode == 1, result.stderr
        assert selection_context.executed() == [prefix + ".test_first"]

    @pytest.mark.parametrize(
        ("selection", "status"),
        [
            ("tests.small.test_selection.MissingCase", 1),
            ("tests.small.test_selection.EmptyCase", 5),
            ("tests.small.domain", 5),
        ],
    )
    def test_missing_or_empty_selection_is_not_success(
        self, selection_context: SelectionContext, selection: str, status: int
    ) -> None:
        """Unknown names fail and empty classes or packages keep the no-tests status."""
        result = selection_context.run(selection)
        assert result.returncode == status, result.stderr
        assert selection_context.executed() == []

    def test_native_node_selects_one_case(self, selection_context: SelectionContext) -> None:
        """A pytest node ID selects its method without running siblings."""
        result = selection_context.run("tests/small/test_selection.py::PassingCase::test_second")
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == [
            "tests.small.test_selection.PassingCase.test_second"
        ]

    def test_native_parameter_node_selects_one_input(
        self, selection_context: SelectionContext
    ) -> None:
        """A parameter ID selects only the named input of a native pytest function."""
        fixture = selection_context.root / "tests/small/test_parameters.py"
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_parameter_function.py",
            fixture,
        )
        result = selection_context.run("tests/small/test_parameters.py::test_value[second]")
        assert result.returncode == 0, result.stderr
        assert selection_context.executed() == ["second"]

    @pytest.mark.parametrize("failfast", [False, True])
    def test_import_failure_returns_failure_and_obeys_failfast(
        self, selection_context: SelectionContext, *, failfast: bool
    ) -> None:
        """An import error fails the tier, and failfast prevents further test execution."""
        fixture = selection_context.root / "tests/small/test_broken.py"
        fixture.write_text("raise RuntimeError('intentional collection-fixture failure')\n")
        arguments = ["--tier", "small", *(["--failfast"] if failfast else [])]
        result = selection_context.run(*arguments, failing_fixture=False)
        assert result.returncode == 1, result.stderr
        assert "intentional collection-fixture failure" in result.stderr
        expected = [
            f"tests.small.{package}test_selection.{case}.{method}"
            for package in ("build.", "domain.", "")
            for case in ("FailureCase", "PassingCase")
            for method in ("test_first", "test_second")
        ]
        assert selection_context.executed() == ([] if failfast else expected)

    def test_child_sigint_returns_shell_interruption_status(
        self, selection_context: SelectionContext
    ) -> None:
        """A SIGINT in pytest itself retains interruption rather than failure status."""
        fixture = selection_context.root / "tests/small/test_interrupt.py"
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_interrupt_case.py",
            fixture,
        )
        result = selection_context.run("tests.small.test_interrupt")
        assert result.returncode == 130, result.stderr
        status = json.loads((selection_context.logs / "tests/run.json").read_text())["status"]
        assert status == "interrupted"

    def test_sigint_returns_shell_interruption_status(
        self, selection_context: SelectionContext
    ) -> None:
        """SIGINT reaches a running pytest process and returns 130 after cleanup."""
        ready = selection_context.root / "ready"
        fixture = selection_context.root / "tests/small/test_wait.py"
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_wait_case.py", fixture
        )

        def interrupt_when_ready(process: subprocess.Popen[str], deadline: float) -> None:
            while not ready.exists():
                assert process.poll() is None, "runner exited before the waiting case"
                assert time.monotonic() < deadline, "waiting case never started"
                time.sleep(0.01)
            os.kill(process.pid, signal.SIGINT)

        result = selection_context.run("tests.small.test_wait", while_running=interrupt_when_ready)
        assert result.returncode == 130, result.stderr
        status = json.loads((selection_context.logs / "tests/run.json").read_text())["status"]
        assert status == "interrupted"


@dataclass
class MixedRuntimeContext:
    """Own controlled selections and external runtime preparation stand-ins."""

    root: Path
    trace: Path
    kern_trace: Path
    driver: Path
    logs: Path = field(init=False)

    def run(
        self, *arguments: str, failing_fixture: bool = False
    ) -> subprocess.CompletedProcess[str]:
        """Execute the combined controller and both actual pytest process boundaries."""
        self.logs = Path(tempfile.mkdtemp(dir=self.root, prefix="logs-"))
        return run_process(
            [sys.executable, str(self.driver), *arguments],
            name="mixed pytest workload",
            cwd=self.root,
            env={
                **python_environment(),
                "FPLINUX_TEST_TRACE": str(self.trace),
                "FPLINUX_TEST_DEPENDENCIES": str(Path(pytest.__file__).parent.parent),
                "FPLINUX_TEST_KERN_TRACE": str(self.kern_trace),
                "FPLINUX_TEST_FAIL": "1" if failing_fixture else "0",
                "FPLINUX_LOG_ROOT": str(self.logs),
                "FPLINUX_LOG_DISPLAY_ROOT": str(self.logs),
                "FPLINUX_VERBOSE": "1" if "--verbose" in arguments else "0",
            },
            timeout=20,
        )

    def executed(self) -> list[str]:
        """Read case outcomes independently of the controller's runtime registry."""
        return self.trace.read_text().splitlines() if self.trace.exists() else []


@pytest.fixture
def mixed_runtime_context(tmp_path: Path) -> MixedRuntimeContext:
    """Own synthetic selections and replace only external runtime preparation."""
    shutil.copyfile(ROOT / "pyproject.toml", tmp_path / "pyproject.toml")
    (tmp_path / "scripts").symlink_to(ROOT / "scripts", target_is_directory=True)
    modules = (
        "tests/host_process/alpine/test_alpine_source_ownership.py",
        "tests/host_process/cache/test_prune_rootfs_ownership.py",
        "tests/host_process/domain/test_selection.py",
        "tests/small/test_selection.py",
    )
    for module in modules:
        path = tmp_path / module
        path.parent.mkdir(parents=True, exist_ok=True)
        for package in path.parents:
            if package == tmp_path:
                break
            (package / "__init__.py").touch()
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/selection_cases.py", path
        )
    driver = tmp_path / "run_workload.py"
    shutil.copyfile(
        ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_workload_driver.py", driver
    )
    fake_kern = tmp_path / "kern-fake"
    install_python_script(
        ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_kern_provider.py",
        fake_kern,
        mode=0o700,
    )
    return MixedRuntimeContext(
        tmp_path, tmp_path / "executed.txt", tmp_path / "kern-launches.txt", driver
    )


class MixedRuntimeProcessTests:
    """Exercise the controller with real pytest, not dependency preparation or real Kern."""

    def test_container_directory_runs_only_its_tests(
        self, mixed_runtime_context: MixedRuntimeContext
    ) -> None:
        """A selected container directory discovers its cases without preparing host tests."""
        result = mixed_runtime_context.run("tests/host_process/domain")

        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == [
            "container:tests.host_process.domain.test_selection.FailureCase.test_first",
            "container:tests.host_process.domain.test_selection.FailureCase.test_second",
            "container:tests.host_process.domain.test_selection.PassingCase.test_first",
            "container:tests.host_process.domain.test_selection.PassingCase.test_second",
        ]
        assert mixed_runtime_context.kern_trace.read_text().splitlines() == ["container"]

    @pytest.mark.parametrize("selection", ["tests/host_process", "tests/host_process/cache"])
    def test_directory_routes_host_cases_once_outside_container(
        self, mixed_runtime_context: MixedRuntimeContext, selection: str
    ) -> None:
        """Directory discovery excludes owned host modules while retaining all selected cases."""
        neighbor = mixed_runtime_context.root / "tests/host_process/cache/test_neighbor.py"
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/selection_cases.py", neighbor
        )

        result = mixed_runtime_context.run(selection)

        assert result.returncode == 0, result.stderr
        modules = [("container", "cache.test_neighbor")]
        if selection == "tests/host_process":
            modules.extend(
                [
                    ("container", "domain.test_selection"),
                    ("host", "alpine.test_alpine_source_ownership"),
                ]
            )
        modules.append(("host", "cache.test_prune_rootfs_ownership"))
        assert mixed_runtime_context.executed() == [
            f"{runtime}:tests.host_process.{module}.{case}.{method}"
            for runtime, module in modules
            for case in ("FailureCase", "PassingCase")
            for method in ("test_first", "test_second")
        ]
        assert mixed_runtime_context.kern_trace.read_text().splitlines() == ["container"]

    def test_tier_workload_runs_gui_and_its_shared_neighbor(
        self, mixed_runtime_context: MixedRuntimeContext
    ) -> None:
        """The outer controller includes GUI components in ordinary tier discovery."""
        _prepare_gui_selection_modules(mixed_runtime_context.root)

        result = mixed_runtime_context.run("--tier", "host_tool")

        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == [
            "container:tests.host_tool.gui.test_desktop_entry.FailureCase.test_first",
            "container:tests.host_tool.gui.test_desktop_entry.FailureCase.test_second",
            "container:tests.host_tool.gui.test_desktop_entry.PassingCase.test_first",
            "container:tests.host_tool.gui.test_desktop_entry.PassingCase.test_second",
            "container:tests.host_tool.gui.test_language_files.FailureCase.test_first",
            "container:tests.host_tool.gui.test_language_files.FailureCase.test_second",
            "container:tests.host_tool.gui.test_language_files.PassingCase.test_first",
            "container:tests.host_tool.gui.test_language_files.PassingCase.test_second",
        ]

    def test_explicit_mixed_repeated_and_overlapping_selections_preserve_order(
        self, mixed_runtime_context: MixedRuntimeContext
    ) -> None:
        """A repeated host method runs again after the intervening container class."""
        host = "tests.host_process.alpine.test_alpine_source_ownership.PassingCase"
        container = "tests.host_process.domain.test_selection.PassingCase"
        other_host = "tests.host_process.cache.test_prune_rootfs_ownership.PassingCase"
        result = mixed_runtime_context.run(
            host + ".test_second",
            container,
            host,
            other_host + ".test_first",
            host + ".test_second",
        )
        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == [
            "host:" + host + ".test_second",
            "container:" + container + ".test_first",
            "container:" + container + ".test_second",
            "host:" + host + ".test_first",
            "host:" + host + ".test_second",
            "host:" + other_host + ".test_first",
            "host:" + host + ".test_second",
        ]

    @pytest.mark.parametrize("interleaved", [False, True])
    def test_adjacent_container_selections_share_launch_until_a_host_selection(
        self, mixed_runtime_context: MixedRuntimeContext, *, interleaved: bool
    ) -> None:
        """Host interleaving splits container execution without reordering selected cases."""
        container = "tests.host_process.domain.test_selection.PassingCase"
        host = "tests.host_process.cache.test_prune_rootfs_ownership.PassingCase.test_first"
        names = [container + ".test_second", container]
        expected = [
            "container:" + container + ".test_second",
            "container:" + container + ".test_first",
            "container:" + container + ".test_second",
        ]
        if interleaved:
            names.insert(1, host)
            expected.insert(1, "host:" + host)
        result = mixed_runtime_context.run(*names)
        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == expected
        assert mixed_runtime_context.kern_trace.read_text().splitlines() == (
            ["container", "container"] if interleaved else ["container"]
        )

    @pytest.mark.parametrize("native", [False, True])
    def test_host_module_selection_routes_without_discovering_other_modules(
        self, mixed_runtime_context: MixedRuntimeContext, *, native: bool
    ) -> None:
        """Dotted and native module selectors reach host pytest with all selected cases."""
        module = "tests.host_process.cache.test_prune_rootfs_ownership"
        selection = "tests/host_process/cache/test_prune_rootfs_ownership.py" if native else module
        result = mixed_runtime_context.run(selection)
        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == [
            "host:" + module + ".FailureCase.test_first",
            "host:" + module + ".FailureCase.test_second",
            "host:" + module + ".PassingCase.test_first",
            "host:" + module + ".PassingCase.test_second",
        ]

    def test_native_parameter_selectors_route_one_input_at_each_boundary(
        self, mixed_runtime_context: MixedRuntimeContext
    ) -> None:
        """Parameter IDs keep punctuation intact while selecting the proper runtime."""
        parameter_case = (
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_parameter_class.py"
        ).read_text()
        for module in (
            "tests/host_process/cache/test_prune_rootfs_ownership.py",
            "tests/host_process/domain/test_selection.py",
        ):
            path = mixed_runtime_context.root / module
            path.write_text(path.read_text() + "\n" + parameter_case)
        result = mixed_runtime_context.run(
            "tests/host_process/cache/test_prune_rootfs_ownership.py::ParameterCase::test_value[other::id]",
            "tests/host_process/domain/test_selection.py::ParameterCase::test_value[first]",
        )
        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == [
            "host:tests.host_process.cache.test_prune_rootfs_ownership.ParameterCase.test_value[other::id]",
            "container:tests.host_process.domain.test_selection.ParameterCase.test_value[first]",
        ]

    def test_tier_discovery_runs_host_cases_once_outside_container(
        self, mixed_runtime_context: MixedRuntimeContext
    ) -> None:
        """Container discovery excludes both host modules without dropping their cases."""
        result = mixed_runtime_context.run("--tier", "host_process")
        assert result.returncode == 0, result.stderr
        assert mixed_runtime_context.executed() == [
            f"{runtime}:tests.host_process.{module}.{case}.{method}"
            for runtime, module in (
                ("container", "domain.test_selection"),
                ("host", "alpine.test_alpine_source_ownership"),
                ("host", "cache.test_prune_rootfs_ownership"),
            )
            for case in ("FailureCase", "PassingCase")
            for method in ("test_first", "test_second")
        ]

    @pytest.mark.parametrize("host_first", [False, True])
    @pytest.mark.parametrize("failfast", [False, True])
    def test_failure_retains_status_and_controls_following_runtime(
        self, mixed_runtime_context: MixedRuntimeContext, *, host_first: bool, failfast: bool
    ) -> None:
        """Either runtime can fail; only failfast cancels the later selected workload."""
        host = "tests.host_process.cache.test_prune_rootfs_ownership"
        container = "tests.host_process.domain.test_selection"
        failing, passing = (host, container) if host_first else (container, host)
        failing_runtime, passing_runtime = (
            ("host", "container")
            if host_first
            else (
                "container",
                "host",
            )
        )
        result = mixed_runtime_context.run(
            failing + ".FailureCase",
            passing + ".PassingCase.test_second",
            *(["--failfast"] if failfast else []),
            failing_fixture=True,
        )
        assert result.returncode == 1, result.stderr
        expected = [f"{failing_runtime}:{failing}.FailureCase.test_first"]
        if not failfast:
            expected.extend(
                [
                    f"{failing_runtime}:{failing}.FailureCase.test_second",
                    f"{passing_runtime}:{passing}.PassingCase.test_second",
                ]
            )
        assert mixed_runtime_context.executed() == expected
        status = json.loads((mixed_runtime_context.logs / "tests/run.json").read_text())["status"]
        assert status == "failed"

    def test_verbose_shows_names_from_host_and_container_pytest(
        self, mixed_runtime_context: MixedRuntimeContext
    ) -> None:
        """Verbose output exposes the selected case at both execution boundaries."""
        result = mixed_runtime_context.run(
            "tests.host_process.cache.test_prune_rootfs_ownership.PassingCase.test_first",
            "tests.host_process.domain.test_selection.PassingCase.test_second",
            "--verbose",
        )
        assert result.returncode == 0, result.stderr
        output = result.stdout + result.stderr
        log = (mixed_runtime_context.logs / "tests/01-selected.log").read_text()
        for selection in (
            "test_prune_rootfs_ownership.py::PassingCase::test_first",
            "test_selection.py::PassingCase::test_second",
        ):
            assert selection in output
            assert selection in log

    @pytest.mark.parametrize("host_interrupt", [False, True])
    def test_child_sigint_interrupts_mixed_workload_without_running_later_case(
        self, mixed_runtime_context: MixedRuntimeContext, *, host_interrupt: bool
    ) -> None:
        """Child pytest SIGINT retains exit 130 and cancels the other runtime."""
        host = "tests/host_process/cache/test_prune_rootfs_ownership.py"
        container = "tests/host_process/domain/test_selection.py"
        interrupted, following = (host, container) if host_interrupt else (container, host)
        shutil.copyfile(
            ROOT / "tests/host_process/quality/fixtures/test-runner/pytest_interrupt_case.py",
            mixed_runtime_context.root / interrupted,
        )
        result = mixed_runtime_context.run(
            interrupted + "::test_interrupt", following + "::PassingCase::test_first"
        )
        assert result.returncode == 130, result.stderr
        assert mixed_runtime_context.executed() == []
        status = json.loads((mixed_runtime_context.logs / "tests/run.json").read_text())["status"]
        assert status == "interrupted"

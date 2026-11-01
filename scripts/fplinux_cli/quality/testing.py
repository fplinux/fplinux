# SPDX-License-Identifier: GPL-2.0-only
"""Select pytest workloads for the full gate and the public test command."""

from __future__ import annotations

import argparse
import os
import sys
import time
from contextlib import ExitStack
from dataclasses import dataclass, field
from itertools import count
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.reporting.run import RunReporter, current_stage, run_entrypoint

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import Literal

TEST_TIERS = {
    "small": 5 * 60,
    "host_process": 10 * 60,
    # Native builds and five-profile application matrices share one deadline.
    "host_tool": 30 * 60,
    "artifact": 300,
    "public_workflow": 90,
}

HOST_TEST_MODULES = (
    "tests/host_process/alpine/test_alpine_source_ownership.py",
    "tests/host_process/cache/test_dependency_environment.py",
    "tests/host_process/cache/test_prune_rootfs_ownership.py",
)

# Pytest's outcome history persists between runs under the managed data root.
_CONTAINER_PYTEST_CACHE = Path("/pytest-cache")


def test_workers(value: str) -> int:
    """Validate the explicit pytest worker limit at the CLI boundary."""
    message = "must be a positive integer"
    try:
        workers = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(message) from error
    if workers < 1:
        raise argparse.ArgumentTypeError(message)
    return workers


def resolved_test_workers(workers: int | None, *, failfast: bool, sanitize: bool) -> int:
    """Choose the default without parallelizing stop-first or sanitizer runs."""
    if workers is not None:
        if not isinstance(workers, int) or isinstance(workers, bool) or workers < 1:
            fail("test workers must be a positive integer")
        if workers > 1 and (failfast or sanitize):
            fail("test workers greater than 1 cannot be combined with --failfast or --sanitize")
        return workers
    if failfast or sanitize:
        return 1
    return min(8, max(1, (os.process_cpu_count() or 1) - 2))


@dataclass(frozen=True)
class _PytestInvocation:
    """Keep each subprocess's selected tests and execution boundary explicit."""

    runtime: Literal["container", "host"]
    command: list[str]
    log_name: str | None = None
    selectors: tuple[str, ...] = ()
    # Pytest resolves all nodes before running a test; one unresolved node runs these instead.
    separately: tuple[_PytestInvocation, ...] = ()


@dataclass
class _SelectionGroup:
    """Adjacent selections that share one pytest process at one execution boundary."""

    runtime: Literal["container", "host"]
    selectors: list[str]
    directories: bool = False
    ignored: list[str] = field(default_factory=list)


def test_name(value: str) -> str:
    """Validate repository selections without importing test modules on the host."""
    if "/" in value or "::" in value:
        path, separator, selected = value.partition("::")
        if separator and "/" not in path:
            message = "use a file path for a pytest node: tests/<tier>/<module>.py::<node>"
            raise argparse.ArgumentTypeError(message)
        identifiers, bracket, parameter = selected.partition("[")
        nodes = identifiers.split("::") if separator else []
        if bracket:
            nodes[-1] += bracket + parameter
        parts = path.removesuffix("/").split("/")
        directory = (
            not separator
            and len(parts) >= 2
            and parts[0] == "tests"
            and parts[1] in TEST_TIERS
            and all(part.replace("-", "_").isidentifier() for part in parts[2:])
        )
        if directory:
            return "/".join(parts)
        if (
            len(parts) >= 3
            and not path.endswith("/")
            and parts[0] == "tests"
            and parts[1] in TEST_TIERS
            and all(part.isidentifier() for part in parts[2:-1])
            and parts[-1].endswith(".py")
            and parts[-1][:-3].isidentifier()
            and all(_valid_node_name(node) for node in nodes)
        ):
            return value
        message = "use tests/<tier>[/<directory>] or tests/<tier>/<module>.py[::<node>]"
        raise argparse.ArgumentTypeError(message)
    parts = value.split(".")
    if len(parts) == 2 and parts[0] == "tests" and parts[1] in TEST_TIERS:
        message = f"use --tier {parts[1]} or tests/{parts[1]} for a whole tier"
        raise argparse.ArgumentTypeError(message)
    if (
        len(parts) < 3
        or parts[0] != "tests"
        or parts[1] not in TEST_TIERS
        or not all(part.isidentifier() for part in parts)
    ):
        message = "use tests.<tier>.<module>[.<class>[.<method>]]"
        raise argparse.ArgumentTypeError(message)
    return value


def _valid_node_name(value: str) -> bool:
    """Allow identifiers and a final pytest parameter ID, never another path or option."""
    identifier, separator, _parameter = value.partition("[")
    return (
        identifier.isidentifier()
        and (not separator or value.endswith("]"))
        and not any(ord(character) < 32 for character in value)
    )


def _pytest_node_id(value: str, workspace: Path = Path()) -> str:
    """Resolve dotted selectors against the captured source workspace."""
    if "/" in value:
        path = value.partition("::")[0]
        if not path.endswith(".py"):
            directory = (workspace / path).resolve()
            if not directory.is_relative_to(workspace.resolve()) or not directory.is_dir():
                fail(f"test directory not found in captured workspace: {path}")
        return value
    parts = value.split(".")
    for length in range(len(parts), 2, -1):
        module_path = Path(*parts[:length])
        module = module_path.with_suffix(".py")
        if (workspace / module).is_file():
            return "::".join((module.as_posix(), *parts[length:]))
        package = module_path / "__init__.py"
        if (workspace / package).is_file():
            return "::".join((package.as_posix(), *parts[length:]))
    return Path(*parts).with_suffix(".py").as_posix()


def add_test_arguments(parser: argparse.ArgumentParser) -> None:
    """Use the same selection contract outside and inside the quality environment."""
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "names",
        nargs="*",
        type=test_name,
        metavar="TEST",
        help=(
            "test directory, dotted module, class or method, or pytest node ID; "
            "omit to discover all test tiers"
        ),
    )
    selection.add_argument("--tier", choices=TEST_TIERS, help="discover only this test tier")
    parser.add_argument(
        "--verbose", action="store_true", help="show test names and stream full logs"
    )
    parser.add_argument(
        "--failfast", action="store_true", help="stop on the first failure or error"
    )
    parser.add_argument(
        "--sanitize",
        action="store_true",
        help="check native tests with GCC ASan and fatal UBSan (no leak checks)",
    )
    parser.add_argument(
        "--workers",
        type=test_workers,
        metavar="N",
        help=(
            "limit parallel host-tool workers (default: up to 8; 1 with --failfast or --sanitize)"
        ),
    )
    history = parser.add_mutually_exclusive_group()
    history.add_argument(
        "--last-failed",
        action="store_true",
        help=(
            "run only the selected tests that failed when last run; "
            "a selection without recorded failures runs completely"
        ),
    )
    history.add_argument(
        "--failed-first",
        action="store_true",
        help="run the selected tests that failed when last run before the others",
    )


def _pytest_command(  # noqa: PLR0913 -- output, stop and history policies stay explicit.
    nodes: list[str],
    *,
    verbose: bool,
    failfast: bool,
    cache_dir: Path,
    last_failed: bool = False,
    failed_first: bool = False,
    workers: int = 1,
    junit: Path | None = None,
) -> list[str]:
    options = (
        (["-v"] if verbose else [])
        + (["-x"] if failfast else [])
        + (["--last-failed"] if last_failed else [])
        + (["--failed-first"] if failed_first else [])
        + (
            [
                "-p",
                "xdist.plugin",
                "-n",
                str(workers),
                "--dist",
                "loadfile",
                "--max-worker-restart=0",
            ]
            if workers > 1
            else []
        )
        + ([f"--junitxml={junit}"] if junit is not None else [])
    )
    prefix = [
        "python3",
        "-m",
        "pytest",
        "--disable-plugin-autoload",
        "--continue-on-collection-errors",
        "-o",
        f"cache_dir={cache_dir}",
    ]
    return [*prefix, *options, *nodes]


def pytest_commands(  # noqa: PLR0913 -- selection, output and instrumentation policies stay explicit.
    names: list[str],
    *,
    cache_dir: Path = _CONTAINER_PYTEST_CACHE,
    tier: str | None = None,
    verbose: bool = False,
    failfast: bool = False,
    workspace: Path = Path(),
    sanitize: bool = False,
    last_failed: bool = False,
    failed_first: bool = False,
    workers: int = 1,
    junit: Path | None = None,
) -> list[tuple[str, list[str], int]]:
    """Use one time budget per selected test tier and retain discovery order."""
    budgets = TEST_TIERS | ({"host_tool": 2700, "host_process": 900} if sanitize else {})

    def command(nodes: list[str]) -> list[str]:
        return _pytest_command(
            nodes,
            verbose=verbose,
            failfast=failfast,
            cache_dir=cache_dir,
            last_failed=last_failed,
            failed_first=failed_first,
            workers=workers,
            junit=junit,
        )

    if names:
        selected_tiers = {name.split("/" if "/" in name else ".")[1] for name in names}
        timeout = sum(budgets[value] for value in selected_tiers)
        nodes = [_pytest_node_id(name, workspace) for name in names]
        return [("selected", command(nodes), timeout)]
    tiers = (tier,) if tier is not None else tuple(TEST_TIERS)
    return [
        (
            name,
            command([f"tests/{name}"]),
            budgets[name],
        )
        for name in tiers
    ]


def pytest_environment() -> dict[str, str]:
    """Keep pytest options and plugins controlled by the pinned project inputs."""
    environment = os.environ.copy()
    environment["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    environment.pop("PYTEST_ADDOPTS", None)
    environment.pop("PYTEST_PLUGINS", None)
    return environment


def _selection_scope(node: str) -> tuple[str, ...]:
    """Name the collected tree that a selection covers, down to a parameter ID."""
    path, _separator, selected = node.partition("::")
    scope = path.removesuffix("/").split("/")
    if scope[-1] == "__init__.py":
        # A package selection may collect its whole directory.
        scope.pop()
    identifiers, bracket, parameter = selected.partition("[")
    if identifiers:
        scope.extend(identifiers.split("::"))
    if bracket:
        scope.append(bracket + parameter)
    return tuple(scope)


def _selections_overlap(first: str, second: str) -> bool:
    """Pytest runs a repeated or nested selection only once within one session."""
    first_scope = _selection_scope(first)
    second_scope = _selection_scope(second)
    common_length = min(len(first_scope), len(second_scope))
    return first_scope[:common_length] == second_scope[:common_length]


def _selected_pytest_groups(names: list[str], *, failfast: bool) -> list[list[str]]:
    """Share a session between adjacent selections without changing their failure policy."""
    groups: list[list[str]] = []
    shared_group: list[str] | None = None
    for name in names:
        node = _pytest_node_id(name)
        # A shared failfast session stops at a later module's collection error before earlier
        # selections finish, so failfast keeps selection-by-selection order.
        if not failfast and Path(node.partition("::")[0]).is_file():
            # Repeated and nested selections need fresh collection to run in requested order.
            if shared_group is None or any(
                _selections_overlap(node, other) for other in shared_group
            ):
                shared_group = []
                groups.append(shared_group)
            shared_group.append(node)
        else:
            groups.append([node])
            shared_group = None
    return groups


def _report_unsuccessful_groups(
    failures: list[tuple[_PytestInvocation, int | str | None]],
) -> None:
    if not failures:
        return
    stage = current_stage()
    entries = []
    for invocation, code in failures:
        selectors = ", ".join(invocation.selectors) or "selected tests"
        entries.append(f"  {invocation.runtime}: {selectors} (exit {code})")
    log = f"\n  log: {stage.display_path}" if stage is not None else ""
    summary = "test: unsuccessful groups:\n" + "\n".join(entries) + log
    if stage is not None:
        stage.write((summary + "\n").encode())
    print(summary, file=sys.stderr, flush=True)


def _run_invocations(
    invocations: list[_PytestInvocation],
    execute: Callable[[_PytestInvocation, float], None],
    *,
    failfast: bool,
    timeout: int,
) -> None:
    """Retain subprocess order and results within one shared workload deadline."""
    deadline = time.monotonic() + timeout
    status = 5
    failures: list[tuple[_PytestInvocation, int | str | None]] = []
    pending = list(invocations)
    try:
        while pending:
            invocation = pending.pop(0)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                fail(f"tests timed out after {timeout}s")
            try:
                execute(invocation, remaining)
            except SystemExit as error:
                if error.code in (2, 130):
                    raise KeyboardInterrupt from None
                if isinstance(error.code, int) and error.code >= 128:
                    raise
                if error.code == 4 and invocation.separately:
                    # The usage error stopped pytest before any selected test ran.
                    pending[:0] = invocation.separately
                    continue
                failures.append((invocation, error.code))
                if error.code != 5:
                    status = 1
                    if failfast:
                        break
            else:
                if status != 1:
                    status = 0
    except BaseException:
        _report_unsuccessful_groups(failures)
        raise
    if status:
        _report_unsuccessful_groups(failures)
        raise SystemExit(status)


def run_test_workload(  # noqa: PLR0913 -- public selection and runtime inputs stay explicit.
    reporter: RunReporter,
    workspace: Path,
    *,
    kern: str,
    rootfs: Path,
    names: list[str],
    tier: str | None = None,
    verbose: bool = False,
    failfast: bool = False,
    sanitize: bool = False,
    last_failed: bool = False,
    failed_first: bool = False,
    cache: Path | None = None,
    workers: int | None = None,
) -> None:
    """Share tier budgets between the quality rootfs and real host namespaces."""
    from .runtime import run_quality_command  # noqa: PLC0415 -- workload-only dependency.

    worker_limit = resolved_test_workers(workers, failfast=failfast, sanitize=sanitize)
    cache = data_root(common.ROOT) if cache is None else cache
    pytest_cache = cache / "pytest-cache"
    pytest_cache.mkdir(parents=True, exist_ok=True)

    if sanitize:
        with reporter.stage("sanitizer-canaries") as stage:
            run_quality_command(
                stage,
                workspace,
                ["python3", "-m", "fplinux_cli.quality.sanitizing"],
                kern=kern,
                rootfs=rootfs,
                timeout=180,
                sanitize=True,
            )
    selected = [_pytest_node_id(name, workspace) for name in names]
    host_modules = [module for module in HOST_TEST_MODULES if (workspace / module).is_file()]
    selected_host_nodes = [
        [node]
        if node.partition("::")[0] in HOST_TEST_MODULES
        else [module for module in host_modules if module.startswith(node + "/")]
        for node in selected
    ]
    needs_host = (
        any(selected_host_nodes)
        if names
        else bool(host_modules) and tier in (None, "host_process")
    )
    emmc_test = "tests/artifact/test_emmc_files.py"
    needs_emmc = (workspace / emmc_test).is_file() and (
        (not names and tier in (None, "artifact"))
        or any(
            node.partition("::")[0] == emmc_test or emmc_test.startswith(node + "/")
            for node in selected
        )
    )
    downloads = None
    if needs_emmc:
        from fplinux_cli.build.host_tools.phone_data import preparation_sources  # noqa: PLC0415

        with reporter.stage("phone-data-inputs"):
            for source in preparation_sources(workspace):
                source.acquire(cache, offline=True)
        downloads = cache / "downloads"
    with ExitStack() as resources:
        host_python: Path | None = None
        host_environment: dict[str, str] | None = None
        if needs_host:
            from .host_testing import (  # noqa: PLC0415 -- host-workload-only dependency.
                host_pytest_environment,
            )

            host_python, host_environment = resources.enter_context(
                host_pytest_environment(workspace, reporter, cache=cache)
            )
        workloads = pytest_commands(
            names,
            tier=tier,
            verbose=verbose,
            failfast=failfast,
            workspace=workspace,
            sanitize=sanitize,
        )
        report_numbers = count(1)
        for label, _command, timeout in workloads:
            with reporter.stage(label, passthrough=True, show_tail=False) as stage:
                host_tool_selection = all(
                    (node == "tests/host_tool" or node.startswith("tests/host_tool/"))
                    and (workspace / node).is_dir()
                    for node in selected
                )
                parallel = (
                    worker_limit > 1
                    and not (last_failed or failed_first)
                    and (host_tool_selection if names else label == "host_tool")
                )

                def invocation(
                    selectors: list[str],
                    *,
                    host: bool,
                    log_name: str | None = None,
                    ignored: tuple[str, ...] = (),
                    worker_count: int = worker_limit if parallel else 1,
                ) -> _PytestInvocation:
                    report_name = f"{stage.name}-{next(report_numbers)}-junit.xml"
                    nodes = list(selectors)
                    for module in ignored:
                        nodes.extend(("--ignore", module))
                    command = _pytest_command(
                        nodes,
                        verbose=verbose,
                        failfast=failfast,
                        cache_dir=pytest_cache if host else _CONTAINER_PYTEST_CACHE,
                        last_failed=last_failed,
                        failed_first=failed_first,
                        workers=worker_count if not host else 1,
                        junit=reporter.root / report_name if host else Path("/logs") / report_name,
                    )
                    return _PytestInvocation(
                        "host" if host else "container", command, log_name, tuple(selectors)
                    )

                if names:
                    groups: list[_SelectionGroup] = []
                    for node, host_nodes in zip(selected, selected_host_nodes, strict=True):
                        previous = groups[-1] if groups else None
                        if (workspace / node).is_dir():
                            # Failfast keeps each directory's collection errors in order.
                            if (
                                not failfast
                                and previous is not None
                                and previous.directories
                                and not any(
                                    _selections_overlap(node, other)
                                    for other in previous.selectors
                                )
                            ):
                                previous.selectors.append(node)
                                previous.ignored.extend(host_nodes)
                            else:
                                groups.append(
                                    _SelectionGroup(
                                        "container",
                                        [node],
                                        directories=True,
                                        ignored=list(host_nodes),
                                    )
                                )
                            if host_nodes:
                                groups.append(_SelectionGroup("host", list(host_nodes)))
                            continue
                        runtime: Literal["container", "host"] = (
                            "host" if node.partition("::")[0] in HOST_TEST_MODULES else "container"
                        )
                        if (
                            runtime == "container"
                            and previous is not None
                            and previous.runtime == runtime
                            and not previous.directories
                        ):
                            previous.selectors.append(node)
                        else:
                            groups.append(_SelectionGroup(runtime, [node]))
                    invocations = []
                    for index, group in enumerate(groups, start=1):
                        if group.runtime == "host":
                            invocations.append(invocation(group.selectors, host=True))
                        elif group.directories:
                            invocations.append(
                                invocation(
                                    group.selectors,
                                    host=False,
                                    log_name=f"{label}-{index}",
                                    ignored=tuple(group.ignored),
                                )
                            )
                        else:
                            command = [
                                "python3",
                                "-m",
                                "fplinux_cli.quality.testing",
                                *group.selectors,
                                "--pytest-cache",
                                str(_CONTAINER_PYTEST_CACHE),
                                "--workers",
                                str(worker_limit if parallel else 1),
                                *(["--verbose"] if verbose else []),
                                *(["--failfast"] if failfast else []),
                                *(["--sanitize"] if sanitize else []),
                                *(["--last-failed"] if last_failed else []),
                                *(["--failed-first"] if failed_first else []),
                            ]
                            invocations.append(
                                _PytestInvocation(
                                    "container",
                                    command,
                                    f"{label}-{index}",
                                    tuple(group.selectors),
                                )
                            )
                else:
                    ignored = tuple(host_modules) if label == "host_process" else ()
                    invocations = [invocation([f"tests/{label}"], host=False, ignored=ignored)]
                    if label == "host_process" and host_modules:
                        invocations.append(invocation(host_modules, host=True))

                def execute(
                    selected: _PytestInvocation,
                    remaining: float,
                    worker_count: int = worker_limit if parallel else 1,
                ) -> None:
                    if selected.runtime == "host":
                        if host_python is None or host_environment is None:
                            fail("host pytest environment was not prepared")
                        stage.run(
                            [str(host_python), *selected.command[1:]],
                            cwd=workspace,
                            env=host_environment,
                            timeout=remaining,
                        )
                    else:
                        run_quality_command(
                            stage,
                            workspace,
                            selected.command,
                            kern=kern,
                            rootfs=rootfs,
                            timeout=remaining,
                            log_name=selected.log_name,
                            downloads=downloads,
                            pytest_cache=pytest_cache,
                            sanitize=sanitize,
                            test_workers=worker_count,
                        )

                _run_invocations(invocations, execute, failfast=failfast, timeout=timeout)


def run_tests(  # noqa: PLR0913 -- public selection, output and history options stay explicit.
    names: list[str],
    *,
    tier: str | None = None,
    verbose: bool = False,
    failfast: bool = False,
    sanitize: bool = False,
    last_failed: bool = False,
    failed_first: bool = False,
    workers: int | None = None,
) -> None:
    """Run the selection and publish full unfiltered success for the check gate."""
    from fplinux_cli.environment.runtime import (  # noqa: PLC0415 -- test-only dependency.
        require_environment,
    )
    from fplinux_cli.workspace.quality_inputs import (  # noqa: PLC0415 -- test-only dependency.
        quality_workspace_snapshot,
    )
    from fplinux_cli.workspace.staging import (  # noqa: PLC0415 -- test-only dependencies.
        discard_staged_quality_workspace_snapshot,
        stage_quality_workspace_snapshot,
    )

    workers = resolved_test_workers(workers, failfast=failfast, sanitize=sanitize)
    cache = data_root(common.ROOT)
    reporter = RunReporter.create("test", target=None, verbose=verbose, selected_data_root=cache)
    with reporter.stage("workspace-snapshot"):
        snapshot = quality_workspace_snapshot(
            enforce_source_policy=False, selected_data_root=cache
        )
    kern, rootfs, state = require_environment(cache=cache)
    success_recipe = None
    if not names and tier is None and not (sanitize or last_failed):
        from .inputs import check_scope_closure_digest  # noqa: PLC0415 -- full-run receipt.
        from .receipts import (  # noqa: PLC0415 -- full-run receipt.
            check_scope_receipt_recipe,
            publish_success_receipt,
        )

        success_recipe = check_scope_receipt_recipe(
            "tests",
            check_scope_closure_digest("tests", snapshot),
            environment_generation=state.generation,
        )
    with reporter.stage("workspace"):
        workspace = stage_quality_workspace_snapshot(snapshot, selected_data_root=cache)
    try:
        run_test_workload(
            reporter,
            workspace,
            kern=kern,
            rootfs=rootfs,
            names=names,
            tier=tier,
            verbose=verbose,
            failfast=failfast,
            sanitize=sanitize,
            last_failed=last_failed,
            failed_first=failed_first,
            cache=cache,
            workers=workers,
        )
        if success_recipe is not None:
            publish_success_receipt(cache, success_recipe)
    finally:
        discard_staged_quality_workspace_snapshot(snapshot, workspace, selected_data_root=cache)
    print("test: OK")
    reporter.finish()


def main() -> None:
    """Execute pytest in the pinned environment with the public command's exit contract."""
    parser = argparse.ArgumentParser()
    add_test_arguments(parser)
    parser.add_argument(
        "--pytest-cache", type=Path, required=True, help="pytest outcome history directory"
    )
    args = parser.parse_args()
    if args.workers is not None and args.workers > 1 and (args.failfast or args.sanitize):
        parser.error("--workers greater than 1 cannot be combined with --failfast or --sanitize")
    reporter = RunReporter.from_environment("test", "tests")
    if reporter is None:
        fail("use ./fplinux test to run tests in the pinned environment")
    environment = pytest_environment()
    report_numbers = count(1)

    def commands(names: list[str], *, report: bool = False) -> list[tuple[str, list[str], int]]:
        # Explicit selectors retain their order and pytest's serial collection-error policy.
        host_tool = not names and args.tier == "host_tool"
        return pytest_commands(
            names,
            cache_dir=args.pytest_cache,
            tier=args.tier,
            verbose=args.verbose,
            failfast=args.failfast,
            sanitize=args.sanitize,
            last_failed=args.last_failed,
            failed_first=args.failed_first,
            workers=(args.workers or 1)
            if host_tool and not (args.last_failed or args.failed_first)
            else 1,
            junit=reporter.root / f"pytest-{next(report_numbers)}.xml" if report else None,
        )

    def selected_invocation(names: list[str]) -> _PytestInvocation:
        [(_label, command, _timeout)] = commands(names, report=True)
        separately = tuple(selected_invocation([name]) for name in names) if len(names) > 1 else ()
        return _PytestInvocation(
            "container", command, selectors=tuple(names), separately=separately
        )

    for label, command, timeout in commands(args.names):
        with reporter.stage(label) as stage:
            if args.names:
                invocations = [
                    selected_invocation(names)
                    for names in _selected_pytest_groups(args.names, failfast=args.failfast)
                ]

                def execute(selected: _PytestInvocation, remaining: float) -> None:
                    stage.run(selected.command, env=environment, timeout=remaining)

                _run_invocations(invocations, execute, failfast=args.failfast, timeout=timeout)
                continue
            try:
                junit = reporter.root / f"pytest-{next(report_numbers)}.xml"
                reported_command = [*command[:-1], f"--junitxml={junit}", command[-1]]
                stage.run(reported_command, env=environment, timeout=timeout)
            except SystemExit as error:
                if error.code == 2:
                    raise KeyboardInterrupt from None
                if error.code in (3, 4, 6):
                    raise SystemExit(1) from None
                raise
    reporter.finish()


if __name__ == "__main__":
    run_entrypoint(main)

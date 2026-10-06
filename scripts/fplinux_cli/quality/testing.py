# SPDX-License-Identifier: GPL-2.0-only
"""Select pytest workloads for the full gate and the public test command."""

from __future__ import annotations

import argparse
import os
import time
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.environment.images import container_image_recipe_digest, load_container_lock
from fplinux_cli.reporting.run import RunReporter, run_entrypoint
from fplinux_cli.workspace.quality_inputs import quality_workspace_snapshot
from fplinux_cli.workspace.staging import (
    discard_staged_quality_workspace_snapshot,
    stage_quality_workspace_snapshot,
)

from .host_testing import host_pytest_environment
from .runtime import prepare_quality_image, run_quality_command

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import Literal

TEST_TIERS = {
    "small": 90,
    "host_process": 180,
    "host_tool": 240,
    "artifact": 300,
    "public_workflow": 90,
}

HOST_TEST_MODULES = (
    "tests/host_process/alpine/test_alpine_source_ownership.py",
    "tests/host_process/cache/test_image_tag.py",
)


@dataclass(frozen=True)
class _PytestInvocation:
    """Keep each subprocess's selected tests and execution boundary explicit."""

    runtime: Literal["container", "host"]
    command: list[str]
    log_name: str | None = None


def test_name(value: str) -> str:
    """Validate repository selections without importing test modules on the host."""
    if "/" in value or "::" in value:
        path, separator, selected = value.partition("::")
        identifiers, bracket, parameter = selected.partition("[")
        nodes = identifiers.split("::") if separator else []
        if bracket:
            nodes[-1] += bracket + parameter
        parts = path.split("/")
        if (
            len(parts) >= 3
            and parts[0] == "tests"
            and parts[1] in TEST_TIERS
            and all(part.isidentifier() for part in parts[2:-1])
            and parts[-1].endswith(".py")
            and parts[-1][:-3].isidentifier()
            and all(_valid_node_name(node) for node in nodes)
        ):
            return value
        message = "use tests/<tier>/<module>.py[::<class>][::<method>[<parameter>]]"
        raise argparse.ArgumentTypeError(message)
    parts = value.split(".")
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
        help="dotted module, class or method, or pytest node ID; omit to discover all test tiers",
    )
    selection.add_argument("--tier", choices=TEST_TIERS, help="discover only this test tier")
    parser.add_argument(
        "--verbose", action="store_true", help="show test names and stream full logs"
    )
    parser.add_argument(
        "--failfast", action="store_true", help="stop on the first failure or error"
    )


def _pytest_command(
    nodes: list[str], *, verbose: bool, failfast: bool, cache_dir: Path
) -> list[str]:
    options = (["-v"] if verbose else []) + (["-x"] if failfast else [])
    prefix = [
        "python3",
        "-m",
        "pytest",
        "--disable-plugin-autoload",
        "--continue-on-collection-errors",
        # Keep pytest's writable cache with this run's logs.
        "-o",
        f"cache_dir={cache_dir}",
    ]
    return [*prefix, *options, *nodes]


def pytest_commands(
    names: list[str], *, tier: str | None = None, verbose: bool = False, failfast: bool = False
) -> list[tuple[str, list[str], int]]:
    """Keep the existing per-tier discovery order and timeout policy."""
    cache_dir = Path(os.environ.get("FPLINUX_LOG_ROOT", ".cache")) / "pytest-cache"

    def command(nodes: list[str]) -> list[str]:
        return _pytest_command(nodes, verbose=verbose, failfast=failfast, cache_dir=cache_dir)

    if names:
        selected_tiers = {name.split("/" if "/" in name else ".")[1] for name in names}
        timeout = sum(TEST_TIERS[value] for value in selected_tiers)
        nodes = [_pytest_node_id(name) for name in names]
        return [("selected", command(nodes), timeout)]
    tiers = (tier,) if tier is not None else tuple(TEST_TIERS)
    return [(name, command([f"tests/{name}"]), TEST_TIERS[name]) for name in tiers]


def pytest_environment() -> dict[str, str]:
    """Keep pytest options and plugins controlled by the pinned project inputs."""
    environment = os.environ.copy()
    environment["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    environment.pop("PYTEST_ADDOPTS", None)
    environment.pop("PYTEST_PLUGINS", None)
    return environment


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
    for invocation in invocations:
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
            if error.code != 5:
                status = 1
                if failfast:
                    break
        else:
            if status != 1:
                status = 0
    if status:
        raise SystemExit(status)


def run_test_workload(  # noqa: PLR0913 -- public selection and runtime inputs stay explicit.
    reporter: RunReporter,
    workspace: Path,
    *,
    kern: str,
    image: str,
    names: list[str],
    tier: str | None = None,
    verbose: bool = False,
    failfast: bool = False,
) -> None:
    """Share tier budgets between the quality image and real host namespaces."""
    selected = [_pytest_node_id(name, workspace) for name in names]
    host_modules = [module for module in HOST_TEST_MODULES if (workspace / module).is_file()]
    needs_host = (
        any(node.partition("::")[0] in HOST_TEST_MODULES for node in selected)
        if names
        else bool(host_modules) and tier in (None, "host_process")
    )
    with ExitStack() as resources:
        host_python: Path | None = None
        host_environment: dict[str, str] | None = None
        if needs_host:
            host_python, host_environment = resources.enter_context(
                host_pytest_environment(workspace, reporter)
            )
        workloads = pytest_commands(names, tier=tier, verbose=verbose, failfast=failfast)
        for label, _command, timeout in workloads:
            with reporter.stage(label, passthrough=True, show_tail=False) as stage:

                def invocation(
                    nodes: list[str],
                    *,
                    host: bool,
                    container_cache_dir: Path = Path("/logs") / label / "pytest-cache",
                ) -> _PytestInvocation:
                    cache_dir = reporter.root / "pytest-cache" if host else container_cache_dir
                    command = _pytest_command(
                        nodes, verbose=verbose, failfast=failfast, cache_dir=cache_dir
                    )
                    return _PytestInvocation("host" if host else "container", command)

                if names:
                    groups: list[tuple[Literal["container", "host"], list[str]]] = []
                    for node in selected:
                        runtime: Literal["container", "host"] = (
                            "host" if node.partition("::")[0] in HOST_TEST_MODULES else "container"
                        )
                        if runtime == "container" and groups and groups[-1][0] == runtime:
                            groups[-1][1].append(node)
                        else:
                            groups.append((runtime, [node]))
                    invocations = []
                    for index, (runtime, nodes) in enumerate(groups, start=1):
                        if runtime == "host":
                            invocations.append(invocation(nodes, host=True))
                        else:
                            command = [
                                "python3",
                                "-m",
                                "fplinux_cli.quality.testing",
                                *nodes,
                                *(["--verbose"] if verbose else []),
                                *(["--failfast"] if failfast else []),
                            ]
                            invocations.append(
                                _PytestInvocation("container", command, f"{label}-{index}")
                            )
                else:
                    nodes = [f"tests/{label}"]
                    if label == "host_process":
                        for module in host_modules:
                            nodes.extend(("--ignore", module))
                    invocations = [invocation(nodes, host=False)]
                    if label == "host_process" and host_modules:
                        invocations.append(invocation(host_modules, host=True))

                def execute(selected: _PytestInvocation, remaining: float) -> None:
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
                            image=image,
                            timeout=remaining,
                            log_name=selected.log_name,
                        )

                _run_invocations(invocations, execute, failfast=failfast, timeout=timeout)


def run_tests(
    names: list[str], *, tier: str | None = None, verbose: bool = False, failfast: bool = False
) -> None:
    """Run selected tests without reading or publishing successful check receipts."""
    reporter = RunReporter.create("test", target=None, verbose=verbose)
    with reporter.stage("workspace-snapshot"):
        snapshot = quality_workspace_snapshot(enforce_source_policy=False)
    lock = load_container_lock()
    kern, image, _state = prepare_quality_image(
        reporter, lock, container_image_recipe_digest(lock)
    )
    with reporter.stage("workspace"):
        workspace = stage_quality_workspace_snapshot(snapshot)
    try:
        run_test_workload(
            reporter,
            workspace,
            kern=kern,
            image=image,
            names=names,
            tier=tier,
            verbose=verbose,
            failfast=failfast,
        )
    finally:
        discard_staged_quality_workspace_snapshot(snapshot, workspace)
    print("test: OK")
    reporter.finish()


def main() -> None:
    """Execute pytest in the pinned environment with the public command's exit contract."""
    parser = argparse.ArgumentParser()
    add_test_arguments(parser)
    args = parser.parse_args()
    reporter = RunReporter.from_environment("test", "tests")
    if reporter is None:
        fail("use ./fplinux test to run tests in the pinned environment")
    environment = pytest_environment()
    for label, command, timeout in pytest_commands(
        args.names, tier=args.tier, verbose=args.verbose, failfast=args.failfast
    ):
        with reporter.stage(label) as stage:
            if args.names:
                invocations = []
                for name in args.names:
                    [(_label, selected_command, _timeout)] = pytest_commands(
                        [name], verbose=args.verbose, failfast=args.failfast
                    )
                    invocations.append(_PytestInvocation("container", selected_command))

                def execute(selected: _PytestInvocation, remaining: float) -> None:
                    stage.run(selected.command, env=environment, timeout=remaining)

                _run_invocations(invocations, execute, failfast=args.failfast, timeout=timeout)
                continue
            try:
                stage.run(command, env=environment, timeout=timeout)
            except SystemExit as error:
                if error.code == 2:
                    raise KeyboardInterrupt from None
                if error.code in (3, 4, 6):
                    raise SystemExit(1) from None
                raise
    reporter.finish()


if __name__ == "__main__":
    run_entrypoint(main)

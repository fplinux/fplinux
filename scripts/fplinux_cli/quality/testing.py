# SPDX-License-Identifier: GPL-2.0-only
"""Select pytest workloads for the full gate and the public test command."""

from __future__ import annotations

import argparse
import os
import time
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

from .runtime import prepare_quality_image, run_quality_command

if TYPE_CHECKING:
    from fplinux_cli.reporting.run import Stage

TEST_TIERS = {
    "small": 90,
    "host_process": 180,
    "host_tool": 240,
    "artifact": 300,
    "public_workflow": 90,
}


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


def _pytest_node_id(value: str) -> str:
    """Resolve dotted selectors against files only after entering the pinned environment."""
    if "/" in value:
        return value
    parts = value.split(".")
    for length in range(len(parts), 2, -1):
        module_path = Path(*parts[:length])
        module = module_path.with_suffix(".py")
        if module.is_file():
            return "::".join((module.as_posix(), *parts[length:]))
        package = module_path / "__init__.py"
        if package.is_file():
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


def pytest_commands(
    names: list[str], *, tier: str | None = None, verbose: bool = False, failfast: bool = False
) -> list[tuple[str, list[str], int]]:
    """Keep the existing per-tier discovery order and timeout policy."""
    options = (["-v"] if verbose else []) + (["-x"] if failfast else [])
    prefix = [
        "python3",
        "-m",
        "pytest",
        "--disable-plugin-autoload",
        "--continue-on-collection-errors",
        # The source workspace is read-only; keep this run's cache with its writable logs.
        "-o",
        f"cache_dir={Path(os.environ.get('FPLINUX_LOG_ROOT', '.cache')) / 'pytest-cache'}",
    ]
    if names:
        selected_tiers = {name.split("/" if "/" in name else ".")[1] for name in names}
        timeout = sum(TEST_TIERS[value] for value in selected_tiers)
        nodes = [_pytest_node_id(name) for name in names]
        return [("selected", [*prefix, *options, *nodes], timeout)]
    tiers = (tier,) if tier is not None else tuple(TEST_TIERS)
    return [(name, [*prefix, *options, f"tests/{name}"], TEST_TIERS[name]) for name in tiers]


def pytest_environment() -> dict[str, str]:
    """Keep pytest options and plugins controlled by the pinned project inputs."""
    environment = os.environ.copy()
    environment["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    environment.pop("PYTEST_ADDOPTS", None)
    environment.pop("PYTEST_PLUGINS", None)
    return environment


def _run_selected_tests(  # noqa: PLR0913 -- timing and output policies stay explicit.
    stage: Stage,
    names: list[str],
    *,
    verbose: bool,
    failfast: bool,
    timeout: int,
    environment: dict[str, str],
) -> None:
    """Retain selector order and repeats within one shared deadline and stage log."""
    deadline = time.monotonic() + timeout
    status = 5
    for name in names:
        [(_label, command, _timeout)] = pytest_commands([name], verbose=verbose, failfast=failfast)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            fail(f"selected tests timed out after {timeout}s")
        try:
            stage.run(command, env=environment, timeout=remaining)
        except SystemExit as error:
            if error.code == 2:
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


def run_tests(
    names: list[str], *, tier: str | None = None, verbose: bool = False, failfast: bool = False
) -> None:
    """Run selected tests in Kern without reading or publishing successful check receipts."""
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
        arguments = ["python3", "-m", "fplinux_cli.quality.testing", *names]
        if tier is not None:
            arguments.extend(("--tier", tier))
        if verbose:
            arguments.append("--verbose")
        if failfast:
            arguments.append("--failfast")
        with reporter.stage("tests", passthrough=True, show_tail=False) as stage:
            run_quality_command(stage, workspace, arguments, kern=kern, image=image)
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
                _run_selected_tests(
                    stage,
                    args.names,
                    verbose=args.verbose,
                    failfast=args.failfast,
                    timeout=timeout,
                    environment=environment,
                )
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

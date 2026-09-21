# SPDX-License-Identifier: GPL-2.0-only
"""Select unittest workloads for the full gate and the public test command."""

from __future__ import annotations

import argparse

from fplinux_cli.common import fail
from fplinux_cli.environment.images import container_image_recipe_digest, load_container_lock
from fplinux_cli.output import RunReporter, run_entrypoint
from fplinux_cli.workspace import (
    discard_staged_quality_workspace_snapshot,
    quality_workspace_snapshot,
    stage_quality_workspace_snapshot,
)

from .runtime import prepare_quality_image, run_quality_command

TEST_TIERS = {
    "small": 90,
    "host_process": 180,
    "host_tool": 240,
    "artifact": 300,
    "public_workflow": 90,
}


def test_name(value: str) -> str:
    """Accept a dotted repository module, class or method without importing it on the host."""
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


def add_test_arguments(parser: argparse.ArgumentParser) -> None:
    """Use the same selection contract outside and inside the quality environment."""
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "names",
        nargs="*",
        type=test_name,
        metavar="TEST",
        help="dotted module, class or method; omit to discover all test tiers",
    )
    selection.add_argument("--tier", choices=TEST_TIERS, help="discover only this test tier")
    parser.add_argument(
        "--verbose", action="store_true", help="show test names and stream full logs"
    )
    parser.add_argument(
        "--failfast", action="store_true", help="stop on the first failure or error"
    )


def unittest_commands(
    names: list[str], *, tier: str | None = None, verbose: bool = False, failfast: bool = False
) -> list[tuple[str, list[str], int]]:
    """Keep the existing per-tier discovery order and timeout policy."""
    options = (["--verbose"] if verbose else []) + (["--failfast"] if failfast else [])
    prefix = ["python3", "-m", "unittest"]
    if names:
        timeout = sum(TEST_TIERS[value] for value in {name.split(".")[1] for name in names})
        return [("selected", [*prefix, *options, *names], timeout)]
    tiers = (tier,) if tier is not None else tuple(TEST_TIERS)
    return [
        (name, [*prefix, "discover", "-s", f"tests/{name}", "-t", ".", *options], TEST_TIERS[name])
        for name in tiers
    ]


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
    """Execute unittest in the pinned environment using its native selection and exit codes."""
    parser = argparse.ArgumentParser()
    add_test_arguments(parser)
    args = parser.parse_args()
    reporter = RunReporter.from_environment("test", "tests")
    if reporter is None:
        fail("use ./fplinux test to run tests in the pinned environment")
    for label, command, timeout in unittest_commands(
        args.names, tier=args.tier, verbose=args.verbose, failfast=args.failfast
    ):
        with reporter.stage(label) as stage:
            stage.run(command, timeout=timeout)
    reporter.finish()


if __name__ == "__main__":
    run_entrypoint(main)

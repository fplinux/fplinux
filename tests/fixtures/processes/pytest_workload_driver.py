# SPDX-License-Identifier: GPL-2.0-only
"""Run the production workload with controlled dependency and Kern boundaries."""

from __future__ import annotations

import argparse
import os
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli.environment import kern as kern_runtime
from fplinux_cli.quality import testing
from fplinux_cli.reporting.run import RunReporter, run_entrypoint

if TYPE_CHECKING:
    from collections.abc import Iterator


@contextmanager
def prepared_host(
    workspace: Path,
    reporter: RunReporter,  # noqa: ARG001 -- retain the dependency preparation interface.
) -> Iterator[tuple[Path, dict[str, str]]]:
    """Reuse the executing pytest dependencies without installing a host environment."""
    environment = testing.pytest_environment()
    environment["FPLINUX_TEST_RUNTIME"] = "host"
    environment["PYTHONNOUSERSITE"] = "1"
    environment["PYTHONPATH"] = os.pathsep.join(
        (str(workspace / "scripts"), os.environ["FPLINUX_TEST_DEPENDENCIES"])
    )
    yield Path(sys.executable), environment


def main() -> None:
    """Keep controller execution real and runtime state inside the temporary workspace."""
    parser = argparse.ArgumentParser()
    testing.add_test_arguments(parser)
    args = parser.parse_args()
    # The provider owns state in the temporary workspace, including Kern's log directory.
    with (
        mock.patch.object(testing, "host_pytest_environment", prepared_host),
        mock.patch.object(kern_runtime, "ROOT", Path.cwd()),
    ):
        reporter = RunReporter.from_environment("test", "tests")
        assert reporter is not None
        testing.run_test_workload(
            reporter,
            Path.cwd(),
            kern=str(Path.cwd() / "kern-fake"),
            image="controlled-image",
            names=args.names,
            tier=args.tier,
            verbose=args.verbose,
            failfast=args.failfast,
        )
        reporter.finish()


if __name__ == "__main__":
    run_entrypoint(main)

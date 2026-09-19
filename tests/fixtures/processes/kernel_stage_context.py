# SPDX-License-Identifier: GPL-2.0-only
"""Run a SIGTERM-ignoring tool inside a worker's real Stage command group."""

import os
import sys
from pathlib import Path

from fplinux_cli.output import RunReporter


def main() -> None:
    root = Path(sys.argv[1])
    target = sys.argv[2]
    reporter = RunReporter("check", root / "stage-run", "test", verbose=False)
    with reporter.stage("blocked tool") as stage:
        stage.run(
            [
                sys.executable,
                str(Path(__file__).with_name("kernel_ignoring_tool.py")),
                str(root),
                target,
                str(os.getpid()),
            ],
            timeout=20,
        )


if __name__ == "__main__":
    main()

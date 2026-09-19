# SPDX-License-Identifier: GPL-2.0-only
"""Run a supplied fixture command through a real Stage in an isolated process."""

import sys
from pathlib import Path

from fplinux_cli.output import RunReporter


def main() -> None:
    root = Path(sys.argv[1])
    command_name = sys.argv[2]
    stage_name = sys.argv[3]
    child_command = sys.argv[4:]
    reporter = RunReporter(command_name, root / "run", "test", verbose=False)
    with reporter.stage(stage_name) as stage:
        stage.run(child_command, timeout=10)


if __name__ == "__main__":
    main()

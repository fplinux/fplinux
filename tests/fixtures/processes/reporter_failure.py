# SPDX-License-Identifier: GPL-2.0-only
"""Exercise ordinary I/O failures and SIGINT at the command entry boundary."""

import os
import signal
import sys
from pathlib import Path

from fplinux_cli.output import RunReporter, run_entrypoint


def main() -> None:
    """Fail before or inside a stage using the test-owned directory."""
    mode, directory = sys.argv[1:]
    root = Path(directory)
    missing = root / "missing-input"
    if mode == "interrupt":
        os.kill(os.getpid(), signal.SIGINT)
    elif mode == "io-stage":
        reporter = RunReporter("check", root / "run", "test", verbose=False)
        with reporter.stage("read-input"):
            missing.read_bytes()
    elif mode == "io":
        missing.read_bytes()
    else:
        raise ValueError(mode)


if __name__ == "__main__":
    run_entrypoint(main)

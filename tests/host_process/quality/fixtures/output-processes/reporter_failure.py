# SPDX-License-Identifier: GPL-2.0-only
"""Exercise expected failures and SIGINT at the command entry boundary."""

import os
import signal
import sys
from email.message import Message
from pathlib import Path
from urllib.error import HTTPError

from fplinux_cli.reporting.run import RunReporter, run_entrypoint


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
    elif mode in {"http", "http-stage"}:
        error = HTTPError(
            "https://example.invalid/source.tar.gz", 404, "Not Found", Message(), None
        )
        if mode == "http-stage":
            reporter = RunReporter("check", root / "run", "test", verbose=False)
            with reporter.stage("download"):
                raise error
        raise error
    else:
        raise ValueError(mode)


if __name__ == "__main__":
    run_entrypoint(main)

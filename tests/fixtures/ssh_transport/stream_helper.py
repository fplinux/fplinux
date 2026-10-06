# SPDX-License-Identifier: GPL-2.0-only
"""Run a bound SSH stream in a process that can receive an isolated interrupt."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from fplinux_cli.runtime import ssh_transport


def main() -> None:
    """Read the supplied session and stream into the supplied binary destination."""
    session = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    with Path(sys.argv[2]).open("w+b") as destination:
        ssh_transport.stream_remote(session, "exec dd if=/dev/nand", destination, timeout=30)


if __name__ == "__main__":
    main()

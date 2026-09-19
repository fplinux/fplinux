# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: SLF001 -- exercise the real coordinator with bounded cleanup deadlines.
"""Exercise the real context coordinator with two controlled fixture workers."""

import sys
from pathlib import Path

from fplinux_cli import kernelcheck


def main() -> None:
    root = Path(sys.argv[1])
    stage_first = sys.argv[2] == "stage"
    contexts = (("first", None), ("second", None))

    def command(target: str, _profile: str | None) -> list[str]:
        fixture = "kernel_context.py"
        if stage_first and target == "first":
            fixture = "kernel_stage_context.py"
        return [sys.executable, str(Path(__file__).with_name(fixture)), str(root), target]

    kernelcheck._CONTEXT_TERMINATE_TIMEOUT = 0.25
    kernelcheck._CONTEXT_KILL_TIMEOUT = 0.5
    kernelcheck._run_context_processes(contexts, 2, command_for=command)


if __name__ == "__main__":
    main()

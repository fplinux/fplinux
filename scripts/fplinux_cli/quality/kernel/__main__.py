# SPDX-License-Identifier: GPL-2.0-only
"""Run the kernel quality preparation or analysis phase."""

from fplinux_cli.reporting.run import run_entrypoint

from .runtime import main

if __name__ == "__main__":
    run_entrypoint(main)

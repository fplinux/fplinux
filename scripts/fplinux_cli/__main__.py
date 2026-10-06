# SPDX-License-Identifier: GPL-2.0-only
"""Parse the repository-local interface and execute its selected command."""

from __future__ import annotations

from fplinux_cli.cli.dispatch import command_action
from fplinux_cli.cli.lifecycle import execute_command
from fplinux_cli.cli.parser import create_parser
from fplinux_cli.manifests.paths import discover_targets, normalize_profile
from fplinux_cli.reporting.run import run_entrypoint


def main() -> None:
    targets = discover_targets()
    parser, check_parser = create_parser(targets)
    args = parser.parse_args()
    if hasattr(args, "profile") and not (args.command == "check" and args.list_scopes):
        args.profile = normalize_profile(args.profile)
    execute_command(args, command_action(args, check_parser))


if __name__ == "__main__":
    run_entrypoint(main)

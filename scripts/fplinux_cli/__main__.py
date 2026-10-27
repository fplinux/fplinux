# SPDX-License-Identifier: GPL-2.0-only
"""Parse the repository-local interface and execute its selected command."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from fplinux_cli.data_paths import DATA_ENV, data_root  # noqa: E402 -- disable bytecode first.


def main() -> None:
    installation = Path(__file__).resolve().parents[2]
    try:
        selected_root = data_root(installation)
    except (OSError, ValueError) as error:
        raise SystemExit(f"fplinux: {error}") from error
    os.environ[DATA_ENV] = str(selected_root)
    if sys.argv[1:2] == ["data-path"]:
        parser = argparse.ArgumentParser(
            prog="fplinux data-path", description="Print the managed data directory."
        )
        parser.add_argument(
            "--check", action="store_true", help="validate checkout placement before writing data"
        )
        args = parser.parse_args(sys.argv[2:])
        if args.check:
            from fplinux_cli.source_paths import (  # noqa: PLC0415 -- checked accessor only.
                validate_checkout_data_root,
            )

            validate_checkout_data_root(installation, selected_root)
        print(selected_root)
        return

    from fplinux_cli.reporting.run import run_entrypoint  # noqa: PLC0415 -- host commands only.

    run_entrypoint(lambda: _host_main(selected_root))


def _host_main(selected_root: Path) -> None:
    """Load command backends only after selecting the immutable host data root."""
    from fplinux_cli.cli.dispatch import command_action  # noqa: PLC0415 -- host commands only.
    from fplinux_cli.cli.lifecycle import execute_command  # noqa: PLC0415 -- host commands only.
    from fplinux_cli.cli.parser import create_parser  # noqa: PLC0415 -- host commands only.
    from fplinux_cli.manifests.paths import (  # noqa: PLC0415 -- host commands only.
        discover_targets,
        normalize_profile,
    )

    targets = discover_targets()
    parser, check_parser = create_parser(targets)
    args = parser.parse_args()
    if hasattr(args, "profile") and not (args.command == "check" and args.list_scopes):
        args.profile = normalize_profile(args.profile)
    execute_command(args, command_action(args, check_parser), data_directory=selected_root)


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-2.0-only
"""Parse runtime actions for one already-selected immutable bundle."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root

from .data_paths import archive_runtime_root
from .workflow import run_phone


def arguments(bundle_root: Path) -> argparse.Namespace:
    """Keep target discovery and build commands outside the delivered runtime."""
    parser = argparse.ArgumentParser(description="Start FPLinux or connect to its USB session.")
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="start the selected phone or open its existing session")
    console = commands.add_parser("console", help="connect to a running phone")
    verify = commands.add_parser("verify", help="check that the running phone matches this build")
    prepare = commands.add_parser("prepare", help="prepare the selected phone's required data")
    nand = commands.add_parser("nand", help="read the running phone's NAND")
    nand_actions = nand.add_subparsers(dest="nand_action", required=True)
    identify = nand_actions.add_parser("identify", help="identify the fitted NAND chip")
    backup = nand_actions.add_parser("backup", help="save a complete NAND backup")
    backup.add_argument(
        "output", type=Path, help="backup file; a receipt is saved beside it as FILE.json"
    )
    for command in (run, console, verify, prepare, identify, backup):
        command.add_argument(
            "--bundle",
            type=Path,
            default=bundle_root,
            metavar="PATH",
            help="use the prepared runtime bundle in this directory",
        )
        command.add_argument(
            "--data-root",
            type=Path,
            metavar="PATH",
            help="keep launcher state and connection keys here (default: installation data scope)",
        )
        if command is not console:
            command.add_argument(
                "--phone-data",
                type=Path,
                metavar="PATH",
                help="use another personal phone-data directory",
            )
        command.add_argument(
            "--log-root",
            type=Path,
            metavar="PATH",
            help="keep launch logs here (default: installation data scope/logs)",
        )
    for command in (run, prepare):
        command.add_argument(
            "--from-dump",
            type=Path,
            metavar="PATH",
            help="prepare phone data from this NAND or eMMC user-area backup",
        )
        command.add_argument(
            "--events",
            type=Path,
            metavar="PATH",
            help="write fresh-load progress as JSON lines, replacing this file",
        )
        command.add_argument(
            "--loader-trace", action="store_true", help="retain loader protocol diagnostics"
        )
    prepare.add_argument("--replace", action="store_true", help="replace prepared phone data")
    selection = console.add_mutually_exclusive_group()
    selection.add_argument(
        "--phone-data", type=Path, metavar="PATH", help="use another personal phone-data directory"
    )
    selection.add_argument(
        "--allow-other-build",
        action="store_true",
        help="use the saved connection without requiring this RAM image or prepared phone data",
    )
    action = console.add_mutually_exclusive_group()
    action.add_argument(
        "--exec",
        dest="exec_command",
        metavar="COMMAND",
        help="run one command on the phone",
    )
    action.add_argument(
        "--upload",
        nargs=2,
        metavar=("LOCAL", "REMOTE"),
        help=argparse.SUPPRESS if os.name == "nt" else "copy one file to an absolute phone path",
    )
    action.add_argument(
        "--pull",
        nargs=2,
        metavar=("REMOTE", "LOCAL"),
        help=argparse.SUPPRESS if os.name == "nt" else "copy one file from an absolute phone path",
    )
    return parser.parse_args()


def main(*, bundle_root: Path) -> None:
    """Invoke the same workflow from checkout selection and archive launchers."""
    options = arguments(bundle_root)
    bundle = options.bundle.resolve()
    try:
        if options.data_root is not None and options.log_root is not None:
            data = options.data_root.resolve()
            logs = options.log_root.resolve()
        else:
            scope = archive_runtime_root(data_root(bundle), bundle)
            data = options.data_root.resolve() if options.data_root is not None else scope
            logs = options.log_root.resolve() if options.log_root is not None else scope / "logs"
    except (OSError, ValueError) as error:
        fail(str(error))
    phone_data = options.phone_data.resolve() if options.phone_data is not None else None
    if options.command in {"console", "verify"}:
        from .live import console_bundle  # noqa: PLC0415 -- selected runtime action.

        raise SystemExit(
            console_bundle(
                bundle,
                data,
                logs,
                allow_other_build=getattr(options, "allow_other_build", False),
                exec_command=getattr(options, "exec_command", None),
                upload=getattr(options, "upload", None),
                pull=getattr(options, "pull", None),
                phone_data=phone_data,
                verify_only=options.command == "verify",
            )
        )
    raise SystemExit(
        run_phone(
            bundle,
            data,
            logs,
            phone_data=phone_data,
            from_dump=(
                options.from_dump.resolve()
                if getattr(options, "from_dump", None) is not None
                else None
            ),
            prepare_only=options.command == "prepare",
            replace=getattr(options, "replace", False),
            events=getattr(options, "events", None),
            loader_trace=getattr(options, "loader_trace", False),
            nand_action=getattr(options, "nand_action", None),
            nand_output=options.output.resolve()
            if getattr(options, "output", None) is not None
            else None,
        )
    )

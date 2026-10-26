# SPDX-License-Identifier: GPL-2.0-only
"""Parse runtime actions for one already-selected immutable bundle."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from .workflow import run_phone


def arguments(bundle_root: Path) -> argparse.Namespace:
    """Keep target discovery and build commands outside the delivered runtime."""
    parser = argparse.ArgumentParser(description="Start FPLinux or reconnect to its USB session.")
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="start the selected phone or open its existing session")
    prepare = commands.add_parser("prepare", help="prepare the selected phone's required data")
    nand = commands.add_parser("nand", help="read the running phone's NAND")
    nand_actions = nand.add_subparsers(dest="nand_action", required=True)
    identify = nand_actions.add_parser("identify", help="identify the fitted NAND chip")
    backup = nand_actions.add_parser("backup", help="save a complete NAND backup")
    backup.add_argument(
        "output", type=Path, help="backup file; a receipt is saved beside it as FILE.json"
    )
    for command in (run, prepare, identify, backup):
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
            help="keep launcher state and connection keys here (default: bundle/data)",
        )
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
            help="keep launch logs here (default: bundle/logs)",
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
    run.add_argument("--reconnect", action="store_true", help="require an existing ready session")
    action = run.add_mutually_exclusive_group()
    action.add_argument(
        "--exec",
        dest="exec_command",
        metavar="COMMAND",
        help="run one command on the phone; requires --reconnect",
    )
    action.add_argument(
        "--upload",
        nargs=2,
        metavar=("LOCAL", "REMOTE"),
        help=argparse.SUPPRESS
        if os.name == "nt"
        else "copy one file to an absolute phone path; requires --reconnect",
    )
    action.add_argument(
        "--pull",
        nargs=2,
        metavar=("REMOTE", "LOCAL"),
        help=argparse.SUPPRESS
        if os.name == "nt"
        else "copy one file from an absolute phone path; requires --reconnect",
    )
    options = parser.parse_args()
    if options.command == "run":
        if not options.reconnect and any(
            value is not None for value in (options.exec_command, options.upload, options.pull)
        ):
            parser.error("--exec, --upload and --pull require --reconnect")
        if options.reconnect and any((options.events, options.loader_trace, options.from_dump)):
            parser.error("--events, --loader-trace and --from-dump require a fresh launch")
    return options


def main(*, bundle_root: Path) -> None:
    """Invoke the same workflow from checkout selection and archive launchers."""
    options = arguments(bundle_root)
    bundle = options.bundle.resolve()
    data = (options.data_root or bundle / "data").resolve()
    logs = (options.log_root or bundle / "logs").resolve()
    raise SystemExit(
        run_phone(
            bundle,
            data,
            logs,
            phone_data=options.phone_data.resolve() if options.phone_data is not None else None,
            from_dump=(
                options.from_dump.resolve()
                if getattr(options, "from_dump", None) is not None
                else None
            ),
            prepare_only=options.command == "prepare",
            replace=getattr(options, "replace", False),
            reconnect=getattr(options, "reconnect", False),
            exec_command=getattr(options, "exec_command", None),
            upload=getattr(options, "upload", None),
            pull=getattr(options, "pull", None),
            events=getattr(options, "events", None),
            loader_trace=getattr(options, "loader_trace", False),
            nand_action=getattr(options, "nand_action", None),
            nand_output=options.output.resolve()
            if getattr(options, "output", None) is not None
            else None,
        )
    )

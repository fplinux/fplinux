# SPDX-License-Identifier: GPL-2.0-only
"""Parse and execute runtime actions for an immutable bundle."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root

from .bundle import MICROSD_BOOT_MODE
from .data_paths import archive_runtime_root
from .workflow import run_phone

if TYPE_CHECKING:
    from collections.abc import Callable, Collection

    from .card import SdCard


def argument_parser(
    *,
    bundle_root: Path | None = None,
    targets: Collection[str] | None = None,
    card_targets: Collection[str] = (),
    microsd_targets: Collection[str] = (),
) -> argparse.ArgumentParser:
    """Accept an immutable bundle path or an exact release catalog target."""
    parser = argparse.ArgumentParser(description="Start FPLinux or connect to its USB session.")
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="start the selected phone or open its existing session")
    console = commands.add_parser("console", help="connect to a running phone")
    verify = commands.add_parser("verify", help="check that the running phone matches this build")
    if microsd_targets:
        for command in (run, console, verify):
            command.add_argument(
                "--boot",
                choices=(MICROSD_BOOT_MODE,),
                help="select the system already installed on the phone's microSD",
            )
    prepare = commands.add_parser("prepare", help="prepare the selected phone's required data")
    nand = commands.add_parser("nand", help="read the running phone's NAND")
    nand_actions = nand.add_subparsers(dest="nand_action", required=True)
    identify = nand_actions.add_parser("identify", help="identify the fitted NAND chip")
    backup = nand_actions.add_parser("backup", help="save a complete NAND backup")
    backup.add_argument(
        "output", type=Path, help="backup file; a receipt is saved beside it as FILE.json"
    )
    card_commands: tuple[argparse.ArgumentParser, ...] = ()
    if card_targets:
        card = commands.add_parser("card", help="install a system card through the phone")
        actions = card.add_subparsers(dest="card_action", required=True)
        card_commands = (
            actions.add_parser("inspect", help="identify the card in the RAM reader session"),
            actions.add_parser("install", help="write the release system to the selected card"),
        )
        card_commands[1].add_argument(
            "--erase-card", metavar="CID", help="allow erasing the card with this identifier"
        )
    for command in (run, console, verify, prepare, identify, backup, *card_commands):
        if targets is None:
            command.add_argument(
                "--bundle",
                type=Path,
                default=bundle_root,
                metavar="PATH",
                help="use the prepared runtime bundle in this directory",
            )
        else:
            command.add_argument(
                "--target",
                choices=sorted(card_targets if command in card_commands else targets),
                required=True,
                metavar="MODEL",
                help="select the phone model by its exact target ID",
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
    for command in (run, prepare, *card_commands):
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
    return parser


def main(*, bundle_root: Path) -> None:
    """Invoke the same workflow from checkout selection and archive launchers."""
    options = argument_parser(bundle_root=bundle_root).parse_args()
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
    raise SystemExit(execute(options, bundle=bundle, data=data, logs=logs))


def confirm_card_erase(card: SdCard) -> bool:
    """Ask the interactive user about the exact card that was discovered."""
    try:
        answer = input(f"Erase all data on SD card {card.cid} and install FPLinux? [y/N] ")
    except EOFError:
        return False
    return answer.strip().lower() in {"y", "yes"}


def execute(
    options: argparse.Namespace,
    *,
    bundle: Path,
    data: Path,
    logs: Path,
    confirm_card: Callable[[SdCard], bool] | None = None,
) -> int:
    """Run the selected action with its already-resolved bundle and state paths."""
    phone_data = options.phone_data.resolve() if options.phone_data is not None else None
    if options.command == "card":
        from .card import run_card  # noqa: PLC0415 -- selected runtime action.

        return run_card(
            bundle,
            data,
            logs,
            inspect_only=options.card_action == "inspect",
            erase_card=getattr(options, "erase_card", None),
            confirm=confirm_card,
            phone_data=phone_data,
            from_dump=options.from_dump.resolve() if options.from_dump is not None else None,
            events=options.events,
            loader_trace=options.loader_trace,
        )
    if options.command in {"console", "verify"}:
        from .live import console_bundle  # noqa: PLC0415 -- selected runtime action.

        return console_bundle(
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
    return run_phone(
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

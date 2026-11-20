# SPDX-License-Identifier: GPL-2.0-only
"""Translate terminal arguments into shared runtime requests."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import fail
from fplinux_cli.data_paths import data_root

from .bundle import MICROSD_BOOT_MODE, MICROSD_BOOT_PROFILE
from .data_paths import archive_runtime_root
from .operations import (
    BundlePaths,
    CardRequest,
    LiveRequest,
    NandRequest,
    PrepareRequest,
    RunRequest,
    execute_operation,
    session_selection_error,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Collection

    from .card import SdCard
    from .operations import CardAction, Operation


def _add_phone_data_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--phone-data",
        type=Path,
        metavar="PATH",
        help="use this phone data directory instead of the user's shared target store",
    )


def _add_loader_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--from-dump",
        type=Path,
        metavar="PATH",
        help="prepare phone data from this NAND or eMMC user-area backup",
    )
    parser.add_argument(
        "--events",
        type=Path,
        metavar="PATH",
        help="write fresh-load progress as JSON lines, replacing this file",
    )
    parser.add_argument(
        "--loader-trace",
        action="store_true",
        help="report RAM-loader protocol commands and reply sizes without payload bytes",
    )


def add_session_commands(
    commands: argparse._SubParsersAction[argparse.ArgumentParser],
    *,
    targets: Collection[str] | None,
    profile_type: Callable[[str], str] | None = None,
    keyboard: bool = False,
) -> tuple[argparse.ArgumentParser, argparse.ArgumentParser, argparse.ArgumentParser]:
    """Register the common run/console/verify surface for each terminal entry."""
    run = commands.add_parser(
        "run", help="prepare phone data, start Linux or reopen its SSH connection"
    )
    console = commands.add_parser("console", help="connect to a running phone")
    verify = commands.add_parser("verify", help="check that the running phone matches this build")
    for command in (run, console, verify):
        if targets is not None:
            command.add_argument(
                "target",
                nargs=None if command is run else "?",
                choices=targets,
                metavar="TARGET",
                help="select the phone model by its exact target ID (%(choices)s)",
            )
            boot = command.add_mutually_exclusive_group()
            boot.add_argument(
                "--boot",
                choices=(MICROSD_BOOT_MODE,),
                help="select the system already installed on the phone's microSD",
            )
            if profile_type is not None:
                boot.add_argument(
                    "--profile",
                    type=profile_type,
                    metavar="NAME",
                    help="select this build profile",
                )
            if command is not run:
                command.add_argument(
                    "--card", metavar="CID", help="connect to this enrolled system microSD card"
                )
        _add_phone_data_argument(command)
    _add_loader_arguments(run)
    console.add_argument(
        "--allow-other-build",
        action="store_true",
        help="use the saved connection without requiring a current build or prepared phone data",
    )
    remote = console.add_mutually_exclusive_group()
    if keyboard:
        remote.add_argument("--keyboard", metavar="EVDEV")
    remote.add_argument(
        "--shutdown", action="store_true", help="safely shut down a running microSD system"
    )
    remote.add_argument("--exec", dest="exec_command", metavar="COMMAND", help="run one command")
    remote.add_argument(
        "--upload",
        nargs=2,
        metavar=("LOCAL", "REMOTE"),
        help=argparse.SUPPRESS if os.name == "nt" else "copy one file to an absolute phone path",
    )
    remote.add_argument(
        "--pull",
        nargs=2,
        metavar=("REMOTE", "LOCAL"),
        help=argparse.SUPPRESS if os.name == "nt" else "copy one file from an absolute phone path",
    )
    remote.add_argument("--screenshot", type=Path, metavar="FILE", help="save a PPM screenshot")
    return run, console, verify


def session_request(options: argparse.Namespace) -> RunRequest | LiveRequest:
    """Read the common part of the parsed namespace, excluding checkout-only options."""
    target = getattr(options, "target", None)
    boot = getattr(options, "boot", None)
    if options.command == "run":
        return RunRequest(
            target=target,
            boot=boot,
            phone_data=options.phone_data,
            from_dump=options.from_dump,
            events=options.events,
            loader_trace=options.loader_trace,
        )
    return LiveRequest(
        target=target,
        boot=boot,
        card_cid=getattr(options, "card", None),
        phone_data=options.phone_data,
        verify_only=options.command == "verify",
        allow_other_build=getattr(options, "allow_other_build", False),
        shutdown=getattr(options, "shutdown", False),
        exec_command=getattr(options, "exec_command", None),
        upload=getattr(options, "upload", None),
        pull=getattr(options, "pull", None),
        screenshot=getattr(options, "screenshot", None),
    )


def validate_session_request(
    parser: argparse.ArgumentParser, options: argparse.Namespace, request: RunRequest | LiveRequest
) -> None:
    profile = getattr(options, "profile", None)
    error = session_selection_error(request, system_card_profile=profile == MICROSD_BOOT_PROFILE)
    if error is not None:
        parser.error(error)
    if isinstance(request, LiveRequest) and request.card_cid is not None:
        if profile is not None and profile != MICROSD_BOOT_PROFILE:
            parser.error("--card selects the microsd-uboot profile")
        if getattr(options, "keyboard", None) is not None:
            parser.error("--card cannot be combined with --keyboard")
    if (
        isinstance(request, LiveRequest)
        and request.allow_other_build
        and (profile is not None or getattr(options, "build_type", None) is not None)
    ):
        parser.error("--allow-other-build cannot be combined with --build-type or --profile")


def add_card_commands(
    commands: argparse._SubParsersAction[argparse.ArgumentParser],
    targets: Collection[str],
) -> tuple[argparse.ArgumentParser, ...]:
    """Register identical microSD commands for the checkout and release launcher."""
    if not targets:
        return ()
    card = commands.add_parser("card", help="prepare a microSD system card through the RAM reader")
    actions = card.add_subparsers(dest="card_action", required=True)
    inspect = actions.add_parser("inspect", help="identify the card in the RAM reader session")
    install = actions.add_parser("install", help="write the release system to the selected card")
    update = actions.add_parser(
        "update", help="install a release alongside the currently booted slot"
    )
    rollback = actions.add_parser("rollback", help="select a previously confirmed system build")
    install.add_argument(
        "--erase-card", metavar="CID", help="allow erasing the card with this identifier"
    )
    for command in (update, rollback):
        command.add_argument("--card", metavar="CID", help="select the exact SD card to change")
    rollback.add_argument(
        "--build-id", required=True, metavar="ID", help="previous confirmed build"
    )
    for command in (inspect, install, update, rollback):
        command.add_argument("target", choices=sorted(targets), metavar="TARGET")
        _add_phone_data_argument(command)
        _add_loader_arguments(command)
    return inspect, install, update, rollback


def card_request(options: argparse.Namespace) -> CardRequest:
    """Read a card action without selecting any device or build."""
    return CardRequest(
        target=options.target,
        phone_data=options.phone_data,
        from_dump=options.from_dump,
        events=options.events,
        loader_trace=options.loader_trace,
        action=options.card_action,
        erase_card=getattr(options, "erase_card", None),
        card_cid=getattr(options, "card", None),
        build_id=getattr(options, "build_id", None),
    )


def argument_parser(
    *,
    bundle_root: Path | None = None,
    targets: Collection[str] | None = None,
    card_targets: Collection[str] = (),
) -> argparse.ArgumentParser:
    """Accept an immutable bundle path or an exact release catalog target."""
    parser = argparse.ArgumentParser(description="Start FPLinux or connect to its USB session.")
    commands = parser.add_subparsers(dest="command", required=True)
    session = add_session_commands(commands, targets=targets)
    prepare = commands.add_parser("prepare", help="prepare the selected phone's required data")
    prepare.add_argument("--replace", action="store_true", help="replace prepared phone data")
    nand = commands.add_parser("nand", help="read the running phone's NAND")
    nand_actions = nand.add_subparsers(dest="nand_action", required=True)
    identify = nand_actions.add_parser("identify", help="identify the fitted NAND chip")
    backup = nand_actions.add_parser("backup", help="save a complete NAND backup")
    backup.add_argument(
        "output", type=Path, help="backup file; a receipt is saved beside it as FILE.json"
    )
    card_commands = add_card_commands(commands, card_targets)
    other_commands = (prepare, identify, backup)
    for command in other_commands:
        _add_phone_data_argument(command)
        if targets is not None:
            command.add_argument(
                "--target",
                choices=targets,
                required=True,
                metavar="MODEL",
                help="select the phone model by its exact target ID",
            )
    _add_loader_arguments(prepare)
    for command in (*session, *other_commands, *card_commands):
        if targets is None:
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
        command.add_argument(
            "--log-root",
            type=Path,
            metavar="PATH",
            help="keep launch logs here (default: installation data scope/logs)",
        )
    return parser


def operation_request(parser: argparse.ArgumentParser, options: argparse.Namespace) -> Operation:
    """Read a terminal request without resolving any data or touching the phone."""
    if options.command in {"run", "console", "verify"}:
        request = session_request(options)
        validate_session_request(parser, options, request)
        return request
    target = getattr(options, "target", None)
    if options.command == "nand":
        return NandRequest(
            action=options.nand_action,
            target=target,
            phone_data=options.phone_data,
            output=getattr(options, "output", None),
        )
    if options.command == "prepare":
        return PrepareRequest(
            target=target,
            phone_data=options.phone_data,
            from_dump=options.from_dump,
            events=options.events,
            loader_trace=options.loader_trace,
            replace=options.replace,
        )
    return card_request(options)


def main(*, bundle_root: Path) -> None:
    """Keep the private argv entry carried by existing immutable builds."""
    parser = argument_parser(bundle_root=bundle_root)
    options = parser.parse_args()
    request = operation_request(parser, options)
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
    raise SystemExit(execute_operation(request, BundlePaths(bundle, data, logs)))


def confirm_card_action(card: SdCard, *, action: CardAction) -> bool:
    """Ask the interactive user about the exact card that was discovered."""
    try:
        if action == "install":
            prompt = f"Erase all data on SD card {card.cid} and install FPLinux?"
        else:
            prompt = f"Apply the requested {action} to SD card {card.cid}?"
        answer = input(f"{prompt} [y/N] ")
    except EOFError:
        return False
    return answer.strip().lower() in {"y", "yes"}

# SPDX-License-Identifier: GPL-2.0-only
"""Command-line parser for the repository-local FPLinux interface."""

from __future__ import annotations

import argparse
import os
from functools import partial
from typing import TYPE_CHECKING

from .cachelock import cache_lock
from .commands import build, console_target, package_target, run_target, verify_booted
from .common import ROOT
from .config import discover_targets
from .container import CHECK_SCOPES, check, check_commit_message, doctor, setup
from .output import run_entrypoint
from .prune import prune

if TYPE_CHECKING:
    from collections.abc import Callable


_EXCLUSIVE_CACHE_COMMANDS = frozenset({"build", "check", "setup"})
_SHARED_CACHE_COMMANDS = frozenset({"console", "package", "run", "verify"})
_CHECK_SCOPE_METAVAR = "{" + ",".join(CHECK_SCOPES) + "}"


def _check_scope(value: str) -> str:
    """Validate one optional check scope without treating an empty list as a value."""
    if value not in CHECK_SCOPES:
        choices = ", ".join(repr(scope) for scope in CHECK_SCOPES)
        raise argparse.ArgumentTypeError(f"invalid choice: {value!r} (choose from {choices})")
    return value


def _cache_lock_exclusive(args: argparse.Namespace) -> bool | None:
    """Return the command-wide cache-lock mode, if this invocation needs one."""
    if args.command == "check" and args.list_scopes:
        return None
    if args.command == "prune":
        return True if args.prune_apply else None
    if args.command in _EXCLUSIVE_CACHE_COMMANDS:
        return True
    if args.command in _SHARED_CACHE_COMMANDS:
        return False
    return None


def _dispatch_with_cache_lock(args: argparse.Namespace, action: Callable[[], None]) -> None:
    """Run exactly one dispatch action while its command-wide cache lock is held."""
    exclusive = _cache_lock_exclusive(args)
    if exclusive is None:
        action()
        return

    target = getattr(args, "target", None)
    with cache_lock(
        ROOT / ".cache",
        exclusive=exclusive,
        command=args.command,
        target=target if isinstance(target, str) else None,
    ):
        action()


def _list_check_scopes(check_parser: argparse.ArgumentParser, scopes: list[str]) -> None:
    """Print the fixed check-scope registry without touching cache state."""
    if scopes:
        check_parser.error("--list cannot be combined with scopes")
    for scope in CHECK_SCOPES:
        print(scope)


def _setup_action(*, force: bool) -> None:
    """Prepare the OCI environment while discarding its internal state object."""
    setup(force=force)


def _command_action(
    args: argparse.Namespace,
    check_parser: argparse.ArgumentParser,
) -> Callable[[], None]:
    """Bind parsed command arguments to one deferred command invocation."""
    if args.command == "doctor":
        action = doctor
    elif args.command == "check":
        if args.list_scopes:
            action = partial(_list_check_scopes, check_parser, args.scopes)
        else:
            action = partial(
                check,
                args.scopes,
                verbose=args.verbose,
                no_cache=args.no_cache,
            )
    elif args.command == "setup":
        action = partial(_setup_action, force=args.force)
    elif args.command == "_commit-msg":
        action = partial(check_commit_message, args.message_file)
    elif args.command == "build":
        action = partial(
            build,
            args.target,
            args.jobs,
            verbose=args.verbose,
            offline=args.offline,
        )
    elif args.command == "package":
        action = partial(package_target, args.target, candidate=args.candidate)
    elif args.command == "prune":
        action = partial(prune, json_output=args.prune_json, apply=args.prune_apply)
    elif args.command == "run":
        action = partial(run_target, args.target)
    elif args.command == "console":
        action = partial(
            console_target,
            args.target,
            keyboard=args.keyboard,
            exec_command=args.exec_command,
            upload=args.upload,
            pull=args.pull,
        )
    elif args.command == "verify":
        action = partial(verify_booted, args.target)
    else:
        raise AssertionError(f"unhandled command: {args.command}")
    return action


def main() -> None:
    targets = discover_targets()
    parser = argparse.ArgumentParser(prog="fplinux")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor", help="check the rootless build host")
    check_parser = commands.add_parser("check", help="run the source quality gate")
    check_parser.add_argument(
        "scopes",
        nargs="*",
        type=_check_scope,
        metavar=_CHECK_SCOPE_METAVAR,
    )
    check_parser.add_argument(
        "--list",
        dest="list_scopes",
        action="store_true",
        help="list available check scopes without running checks",
    )
    check_parser.add_argument(
        "--verbose",
        action="store_true",
        help="stream complete stage output while retaining logs",
    )
    check_parser.add_argument(
        "--no-cache",
        action="store_true",
        help="run every selected check even when an exact success receipt exists",
    )
    setup_parser = commands.add_parser("setup", help="build the pinned OCI environment")
    setup_parser.add_argument("--force", action="store_true")
    commit_message_parser = commands.add_parser("_commit-msg", help=argparse.SUPPRESS)
    commit_message_parser.add_argument("message_file")
    build_parser = commands.add_parser("build", help="build a target in .cache/out")
    build_parser.add_argument("target", choices=targets)
    build_parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    build_parser.add_argument(
        "--verbose",
        action="store_true",
        help="stream complete stage output while retaining logs",
    )
    build_parser.add_argument(
        "--offline",
        action="store_true",
        help="on a build miss, run the prepared build image without network access",
    )
    package_parser = commands.add_parser(
        "package", help="package an existing build for Linux x86-64"
    )
    package_parser.add_argument("target", choices=targets)
    package_parser.add_argument(
        "--candidate",
        action="store_true",
        help="create a clearly marked hardware-qualification candidate",
    )
    prune_parser = commands.add_parser(
        "prune", help="show a safe cache-prune inventory or apply it"
    )
    prune_parser.add_argument(
        "--json",
        dest="prune_json",
        action="store_true",
        help="emit the inventory or apply result as JSON",
    )
    prune_parser.add_argument(
        "--apply",
        dest="prune_apply",
        action="store_true",
        help="under the global cache lock, remove disposable staged workspaces",
    )
    run_parser = commands.add_parser("run", help="run a target's volatile-RAM loader")
    run_parser.add_argument("target", choices=targets)

    console_parser = commands.add_parser("console", help="connect to a running target over USB")
    console_parser.add_argument("target", choices=targets)
    console_actions = console_parser.add_mutually_exclusive_group()
    console_actions.add_argument("--keyboard", metavar="EVDEV")
    console_actions.add_argument("--exec", dest="exec_command", metavar="COMMAND")
    console_actions.add_argument("--upload", nargs=2, metavar=("LOCAL", "REMOTE"))
    console_actions.add_argument("--pull", nargs=2, metavar=("REMOTE", "LOCAL"))

    verify_parser = commands.add_parser(
        "verify", help="check that the booted phone runs the current build"
    )
    verify_parser.add_argument("target", choices=targets)
    args = parser.parse_args()
    _dispatch_with_cache_lock(args, _command_action(args, check_parser))


if __name__ == "__main__":
    run_entrypoint(main)

# SPDX-License-Identifier: GPL-2.0-only
"""Command-line parser for the repository-local FPLinux interface."""

from __future__ import annotations

import argparse
import os
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.cli.build import build
from fplinux_cli.cli.bundles import PUBLIC_BOOT_MODES, selected_context_profile
from fplinux_cli.cli.checksum import checksum_aport
from fplinux_cli.cli.inspect import inspect_apk, inspect_archive, inspect_bundle
from fplinux_cli.cli.logs import add_log_arguments, read_logs
from fplinux_cli.cli.package import package_target
from fplinux_cli.cli.probe import build_probe
from fplinux_cli.cli.runtime import console_target, run_target, verify_booted
from fplinux_cli.manifests.paths import GLOBAL_PROFILES, discover_targets, normalize_profile
from fplinux_cli.manifests.values import TARGET_NAME

from .cachelock import cache_lock
from .common import ROOT
from .device_data_prepare import prepare_device_data
from .environment.kern import doctor, setup
from .format import format_sources
from .nand_backup import backup_target_nand, identify_target_nand
from .output import run_entrypoint
from .prune import (
    discard_obsolete_apks,
    discard_obsolete_rootfs,
    discard_superseded_profile_logs,
    prune,
)
from .quality.checks import CHECK_SCOPES, check
from .quality.git import check_commit_message
from .quality.testing import add_test_arguments, run_tests
from .target_new import create_target

if TYPE_CHECKING:
    from collections.abc import Callable


_EXCLUSIVE_CACHE_COMMANDS = frozenset(
    {"build", "check", "checksum", "device-data", "format", "nand", "probe-build", "setup", "test"}
)
_SHARED_CACHE_COMMANDS = frozenset({"console", "package", "run", "verify"})
_CHECK_SCOPE_METAVAR = "{" + ",".join(CHECK_SCOPES) + "}"
_PUBLIC_COMMAND_METAVAR = (
    "{doctor,check,test,logs,inspect,format,setup,build,probe-build,checksum,package,prune,"
    "run,console,nand,device-data,target,verify}"
)


def _check_scope(value: str) -> str:
    """Validate one optional check scope without treating an empty list as a value."""
    if value not in CHECK_SCOPES:
        choices = ", ".join(repr(scope) for scope in CHECK_SCOPES)
        raise argparse.ArgumentTypeError(f"invalid choice: {value!r} (choose from {choices})")
    return value


def _positive_jobs(value: str) -> int:
    """Parse one positive worker limit before it can take the cache lock."""
    try:
        jobs = int(value)
    except ValueError as error:
        message = "must be a positive integer"
        raise argparse.ArgumentTypeError(message) from error
    if jobs < 1:
        message = "must be a positive integer"
        raise argparse.ArgumentTypeError(message)
    return jobs


def _profile_name(value: str) -> str:
    """Accept one of the two public build profiles."""
    if TARGET_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError(f"invalid profile name: {value!r}")
    if value not in GLOBAL_PROFILES:
        raise argparse.ArgumentTypeError(
            f"unknown profile: {value!r} (choose from default, microsd-uboot)"
        )
    return value


def _cache_lock_exclusive(args: argparse.Namespace) -> bool | None:
    """Return the command-wide cache-lock mode, if this invocation needs one."""
    if args.command == "check" and args.list_scopes:
        return None
    if args.command == "prune":
        return True if args.prune_apply else None
    if args.command == "inspect":
        return False if args.inspect_kind == "bundle" else None
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
    profile = selected_context_profile(
        target if isinstance(target, str) else None,
        profile=getattr(args, "profile", None),
        boot=getattr(args, "boot", None),
    )
    with cache_lock(
        ROOT / ".cache",
        exclusive=exclusive,
        command=args.command,
        target=target if isinstance(target, str) else None,
        profile=profile,
    ):
        try:
            action()
        finally:
            _discard_profile_build_or_check_cache(args)


def _discard_profile_build_or_check_cache(args: argparse.Namespace) -> None:
    """Bound one named profile's logs even when its command exits unsuccessfully."""
    profile = getattr(args, "profile", None)
    if not isinstance(profile, str) or args.command not in {"build", "check"}:
        return
    target = getattr(args, "target", None)
    if args.command == "build":
        if not isinstance(target, str):
            message = "build cache retention requires a target"
            raise AssertionError(message)
        discard_obsolete_rootfs(ROOT / ".cache")
        discard_obsolete_apks(ROOT / ".cache")
    discard_superseded_profile_logs(
        ROOT / ".cache",
        args.command,
        profile=profile,
        target=target if isinstance(target, str) else None,
    )


def _list_check_scopes(check_parser: argparse.ArgumentParser, scopes: list[str]) -> None:
    """Print the fixed check-scope registry without touching cache state."""
    if scopes:
        check_parser.error("--list cannot be combined with scopes")
    for scope in CHECK_SCOPES:
        print(scope)


def _setup_action(*, force: bool) -> None:
    """Prepare the OCI environment while discarding its internal state object."""
    setup(force=force)


def _nand_backup_action(target: str, output: Path, *, profile: str | None) -> None:
    """Save a complete NAND image through the selected running system."""
    backup_target_nand(target, output, profile=profile)


def _command_action(
    args: argparse.Namespace,
    check_parser: argparse.ArgumentParser,
) -> Callable[[], None]:
    """Bind parsed command arguments to one deferred command invocation."""
    if args.command == "doctor":
        action = doctor
    elif args.command == "logs":
        action = partial(read_logs, args)
    elif args.command == "inspect":
        if args.inspect_kind == "bundle":
            action = partial(inspect_bundle, args.target, profile=args.profile)
        elif args.inspect_kind == "archive":
            action = partial(inspect_archive, args.path)
        else:
            action = partial(inspect_apk, args.path)
    elif args.command == "check":
        if args.list_scopes:
            if args.profile is not None:
                check_parser.error("--profile cannot be combined with --list")
            if args.jobs is not None:
                check_parser.error("--jobs cannot be combined with --list")
            action = partial(_list_check_scopes, check_parser, args.scopes)
        else:
            jobs = args.jobs
            if jobs is None:
                jobs = 1 if args.verbose or (args.scopes and "kernel" not in args.scopes) else 3
            if jobs > 1 and args.verbose:
                check_parser.error("--verbose cannot be combined with --jobs greater than 1")
            if jobs > 1 and args.scopes and "kernel" not in args.scopes:
                check_parser.error("--jobs greater than 1 requires the kernel check scope")
            action = partial(
                check,
                args.scopes,
                profile=args.profile,
                verbose=args.verbose,
                no_cache=args.no_cache,
                jobs=jobs,
            )
    elif args.command == "setup":
        action = partial(_setup_action, force=args.force)
    elif args.command == "test":
        action = partial(
            run_tests, args.names, tier=args.tier, verbose=args.verbose, failfast=args.failfast
        )
    elif args.command == "format":
        action = partial(format_sources, args.paths)
    elif args.command == "_commit-msg":
        action = partial(check_commit_message, args.message_file)
    elif args.command == "build":
        action = partial(
            build,
            args.target,
            args.jobs,
            profile=args.profile,
            verbose=args.verbose,
            offline=args.offline,
        )
    elif args.command == "checksum":
        action = partial(checksum_aport, args.aport, offline=args.offline)
    elif args.command == "probe-build":
        action = partial(build_probe, args.source, output=args.output)
    elif args.command == "package":
        action = partial(
            package_target,
            args.target,
            profile=args.profile,
            boot=args.boot,
            candidate=args.candidate,
        )
    elif args.command == "prune":
        action = partial(prune, apply=args.prune_apply)
    elif args.command == "run":
        action = partial(
            run_target,
            args.target,
            profile=args.profile,
            boot=args.boot,
        )
    elif args.command == "console":
        action = partial(
            console_target,
            args.target,
            profile=args.profile,
            keyboard=args.keyboard,
            exec_command=args.exec_command,
            upload=args.upload,
            pull=args.pull,
        )
    elif args.command == "nand":
        if args.nand_command == "backup":
            action = partial(_nand_backup_action, args.target, args.output, profile=args.profile)
        elif args.nand_command == "identify":
            action = partial(identify_target_nand, args.target, profile=args.profile)
        else:
            raise AssertionError(f"unhandled nand command: {args.nand_command}")
    elif args.command == "device-data":
        if args.device_data_command != "prepare":
            raise AssertionError(f"unhandled device-data command: {args.device_data_command}")
        action = partial(
            prepare_device_data,
            args.target,
            from_dump=args.from_dump,
            jobs=args.jobs,
            offline=args.offline,
        )
    elif args.command == "target":
        if args.target_command != "new":
            raise AssertionError(f"unhandled target command: {args.target_command}")
        action = partial(
            create_target,
            args.name,
            platform=args.platform,
            brand=args.brand,
            product=args.product,
            compatible=args.compatible,
        )
    elif args.command == "verify":
        action = partial(verify_booted, args.target, profile=args.profile)
    else:
        raise AssertionError(f"unhandled command: {args.command}")
    return action


def _add_boot_profile_options(parser: argparse.ArgumentParser, verb: str) -> None:
    context = parser.add_mutually_exclusive_group()
    context.add_argument(
        "--boot",
        choices=PUBLIC_BOOT_MODES,
        help=f"{verb} the selected public boot mode",
    )
    context.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help=f"{verb} the selected global profile",
    )


def _add_device_data_prepare_command(
    parser: argparse.ArgumentParser,
    targets: tuple[str, ...],
) -> None:
    """Add the canonical fitted-data preparation command."""
    commands = parser.add_subparsers(
        dest="device_data_command",
        required=True,
        metavar="{prepare}",
    )
    prepare = commands.add_parser(
        "prepare",
        help="extract all declared device data from one physical NAND backup",
    )
    prepare.add_argument("target", choices=targets)
    prepare.add_argument(
        "--from-dump",
        type=Path,
        metavar="PATH",
        help="use an existing physical NAND backup without connecting the phone",
    )
    prepare.add_argument(
        "--jobs",
        type=_positive_jobs,
        default=max(1, os.cpu_count() or 1),
        metavar="N",
        help="limit jobs when the read-only NAND loader must be built",
    )
    prepare.add_argument(
        "--offline",
        action="store_true",
        help="build the read-only NAND loader without network access",
    )


def main() -> None:
    targets = discover_targets()
    parser = argparse.ArgumentParser(prog="fplinux")
    commands = parser.add_subparsers(
        dest="command",
        required=True,
        metavar=_PUBLIC_COMMAND_METAVAR,
    )
    commands.add_parser("doctor", help="check the project-local build runtime")
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
    check_parser.add_argument(
        "--jobs",
        type=_positive_jobs,
        default=None,
        metavar="N",
        help="limit concurrent kernel check contexts (default: 3, or 1 with --verbose)",
    )
    check_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="check the selected global profile (default: default)",
    )
    test_parser = commands.add_parser(
        "test", help="run selected unittest tests in the pinned environment"
    )
    add_test_arguments(test_parser)
    logs_parser = commands.add_parser("logs", help="list, inspect or follow recorded command logs")
    add_log_arguments(logs_parser)
    inspect_parser = commands.add_parser("inspect", help="inspect a built bundle, ZIP or APK")
    inspections = inspect_parser.add_subparsers(dest="inspect_kind", required=True)
    bundle_parser = inspections.add_parser("bundle", help="inspect the current target bundle")
    bundle_parser.add_argument("target", choices=targets)
    bundle_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    for kind, help_text in (
        ("archive", "check and inspect a FPLinux ZIP"),
        ("apk", "read APK metadata and file list"),
    ):
        item_parser = inspections.add_parser(kind, help=help_text)
        item_parser.add_argument("path", type=Path, metavar="PATH")
    format_parser = commands.add_parser(
        "format", help="format explicit project sources in the pinned environment"
    )
    format_parser.add_argument(
        "paths",
        nargs="+",
        metavar="PATH",
        help="normalized repository-relative source path or declared Linux patch",
    )
    setup_parser = commands.add_parser("setup", help="build the pinned OCI environment")
    setup_parser.add_argument(
        "--force",
        action="store_true",
        help="rebuild the pinned image even when the current recipe is ready",
    )
    commit_message_parser = commands.add_parser("_commit-msg")
    commit_message_parser.add_argument("message_file")
    build_parser = commands.add_parser("build", help="build a target in .cache/out")
    build_parser.add_argument("target", choices=targets)
    build_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="build one global profile (default: default)",
    )
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
    probe_parser = commands.add_parser(
        "probe-build", help="build one static ARMv7 hard-float musl C probe"
    )
    probe_parser.add_argument(
        "source", metavar="SOURCE.c", help="normalized repository-relative C source path"
    )
    probe_parser.add_argument(
        "--output",
        required=True,
        metavar=".cache/tools/NAME",
        help="normalized repository-relative output path inside .cache/tools",
    )
    checksum_parser = commands.add_parser(
        "checksum",
        help="regenerate one Alpine aport sha512sums block in the pinned build image",
    )
    checksum_parser.add_argument("aport")
    checksum_parser.add_argument(
        "--offline",
        action="store_true",
        help="regenerate only from the prepared Alpine source cache without network access",
    )
    package_parser = commands.add_parser(
        "package", help="package an existing build for Linux x86-64"
    )
    package_parser.add_argument("target", choices=targets)
    _add_boot_profile_options(package_parser, "package")
    package_parser.add_argument(
        "--candidate",
        action="store_true",
        help="create a clearly marked phone-test candidate",
    )
    prune_parser = commands.add_parser(
        "prune", help="show a safe cache-prune inventory or apply it"
    )
    prune_parser.add_argument(
        "--apply",
        dest="prune_apply",
        action="store_true",
        help="under the global cache lock, remove disposable staged workspaces",
    )
    run_parser = commands.add_parser("run", help="run a target's volatile-RAM loader")
    run_parser.add_argument("target", choices=targets)
    _add_boot_profile_options(run_parser, "run")

    console_parser = commands.add_parser("console", help="connect to a running target over USB")
    console_parser.add_argument("target", choices=targets)
    console_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="reconnect to a session started from the selected global profile",
    )
    console_actions = console_parser.add_mutually_exclusive_group()
    console_actions.add_argument("--keyboard", metavar="EVDEV")
    console_actions.add_argument("--exec", dest="exec_command", metavar="COMMAND")
    console_actions.add_argument("--upload", nargs=2, metavar=("LOCAL", "REMOTE"))
    console_actions.add_argument("--pull", nargs=2, metavar=("REMOTE", "LOCAL"))

    nand_parser = commands.add_parser("nand", help="read the target's internal NAND")
    nand_commands = nand_parser.add_subparsers(dest="nand_command", required=True)
    nand_backup_parser = nand_commands.add_parser(
        "backup", help="save a complete read-only NAND image"
    )
    nand_backup_parser.add_argument("target", choices=targets)
    nand_backup_parser.add_argument("output", type=Path)
    nand_backup_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    nand_identify_parser = nand_commands.add_parser(
        "identify", help="print the NAND chip identity and geometry reported by the phone"
    )
    nand_identify_parser.add_argument("target", choices=targets)
    nand_identify_parser.add_argument("--profile", type=_profile_name, metavar="NAME")

    device_data_parser = commands.add_parser(
        "device-data", help="prepare target-owned fitted data from one NAND image"
    )
    _add_device_data_prepare_command(device_data_parser, targets)

    target_parser = commands.add_parser("target", help="create a phone target")
    target_commands = target_parser.add_subparsers(
        dest="target_command",
        required=True,
        metavar="{new}",
    )
    new_target_parser = target_commands.add_parser(
        "new", help="create a headless target for a phone without one"
    )
    new_target_parser.add_argument(
        "name", metavar="TARGET", help="lowercase target name, for example brand-model"
    )
    new_target_parser.add_argument(
        "--platform",
        metavar="NAME",
        help="SoC platform under platforms/ (default: the only one; required when several exist)",
    )
    new_target_parser.add_argument("--brand", required=True, help="public brand name")
    new_target_parser.add_argument("--product", required=True, help="public product name")
    new_target_parser.add_argument(
        "--compatible",
        metavar="VENDOR,DEVICE",
        help="exact board compatible (default: derived from the brand and product)",
    )

    verify_parser = commands.add_parser(
        "verify", help="check that the booted phone runs the current build"
    )
    verify_parser.add_argument("target", choices=targets)
    verify_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    args = parser.parse_args()
    if hasattr(args, "profile") and not (args.command == "check" and args.list_scopes):
        args.profile = normalize_profile(args.profile)
    _dispatch_with_cache_lock(args, _command_action(args, check_parser))


if __name__ == "__main__":
    run_entrypoint(main)

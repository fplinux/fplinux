# SPDX-License-Identifier: GPL-2.0-only
"""Bind validated command arguments to one deferred operation."""

from __future__ import annotations

from functools import partial
from typing import TYPE_CHECKING

from fplinux_cli.cache.prune.operations import prune
from fplinux_cli.cli.build import build
from fplinux_cli.cli.checksum import checksum_aport
from fplinux_cli.cli.dependencies import (
    create_dependencies,
    restore_dependencies,
    verify_dependencies,
)
from fplinux_cli.cli.device_data import prepare_device_data
from fplinux_cli.cli.inspect import (
    inspect_apk,
    inspect_archive,
    inspect_bundle,
    inspect_footprint_diff,
    inspect_target_footprint,
)
from fplinux_cli.cli.logs import read_logs
from fplinux_cli.cli.package import package_target
from fplinux_cli.cli.probe import build_probe
from fplinux_cli.cli.runtime import console_target, verify_booted
from fplinux_cli.cli.target_new import create_target
from fplinux_cli.environment.doctor import doctor
from fplinux_cli.environment.setup import setup
from fplinux_cli.quality.checks import check
from fplinux_cli.quality.formatting.command import format_sources
from fplinux_cli.quality.git import check_commit_message
from fplinux_cli.quality.scopes import CHECK_SCOPES
from fplinux_cli.quality.testing import run_tests
from fplinux_cli.runtime.nand_backup import backup_target_nand, identify_target_nand
from fplinux_cli.runtime.runner import run_target

if TYPE_CHECKING:
    import argparse
    from collections.abc import Callable
    from pathlib import Path


def _list_check_scopes(check_parser: argparse.ArgumentParser, scopes: list[str]) -> None:
    """Print the fixed check-scope registry without touching cache state."""
    if scopes:
        check_parser.error("--list cannot be combined with scopes")
    for scope in CHECK_SCOPES:
        print(scope)


def _setup_action(*, force: bool, offline: bool) -> None:
    """Prepare the OCI environment while discarding its internal state object."""
    if offline:
        setup(force=force, offline=True)
    else:
        setup(force=force)


def _nand_backup_action(
    target: str, output: Path, *, profile: str | None, build_type: str
) -> None:
    """Save a complete NAND image through the selected running system."""
    backup_target_nand(target, output, profile=profile, build_type=build_type)


def command_action(
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
            action = partial(
                inspect_bundle, args.target, profile=args.profile, build_type=args.build_type
            )
        elif args.inspect_kind == "archive":
            action = partial(inspect_archive, args.path)
        elif args.inspect_kind == "footprint":
            action = partial(
                inspect_target_footprint,
                args.target,
                profile=args.profile,
                build_type=args.build_type,
                json_output=args.json,
            )
        elif args.inspect_kind == "footprint-diff":
            action = partial(
                inspect_footprint_diff, args.before, args.after, json_output=args.json
            )
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
                build_type=args.build_type,
                verbose=args.verbose,
                no_cache=args.no_cache,
                jobs=jobs,
            )
    elif args.command == "setup":
        action = partial(_setup_action, force=args.force, offline=args.offline)
    elif args.command == "dependencies":
        if args.dependencies_command == "create":
            action = partial(
                create_dependencies,
                args.directory,
                offline=args.offline,
                sources=args.sources,
                inputs_only=args.inputs_only,
            )
        elif args.dependencies_command == "verify":
            action = partial(verify_dependencies, args.directory)
        else:
            action = partial(restore_dependencies, args.directory, inputs_only=args.inputs_only)
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
            build_type=args.build_type,
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
            build_type=args.build_type,
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
            build_type=args.build_type,
            boot=args.boot,
            events=args.events,
        )
    elif args.command == "console":
        action = partial(
            console_target,
            args.target,
            profile=args.profile,
            build_type=args.build_type,
            keyboard=args.keyboard,
            exec_command=args.exec_command,
            upload=args.upload,
            pull=args.pull,
        )
    elif args.command == "nand":
        if args.nand_command == "backup":
            action = partial(
                _nand_backup_action,
                args.target,
                args.output,
                profile=args.profile,
                build_type=args.build_type,
            )
        elif args.nand_command == "identify":
            action = partial(
                identify_target_nand, args.target, profile=args.profile, build_type=args.build_type
            )
        else:
            raise AssertionError(f"unhandled nand command: {args.nand_command}")
    elif args.command == "device-data":
        if args.device_data_command != "prepare":
            raise AssertionError(f"unhandled device-data command: {args.device_data_command}")
        action = partial(
            prepare_device_data,
            args.target,
            from_dump=args.from_dump,
            events=args.events,
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
        action = partial(
            verify_booted, args.target, profile=args.profile, build_type=args.build_type
        )
    else:
        raise AssertionError(f"unhandled command: {args.command}")
    return action

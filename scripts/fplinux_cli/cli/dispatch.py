# SPDX-License-Identifier: GPL-2.0-only
"""Bind validated command arguments to one deferred operation."""

from __future__ import annotations

from functools import partial
from typing import TYPE_CHECKING, cast

from fplinux_cli.common import fail
from fplinux_cli.quality.scopes import CHECK_SCOPES

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
    """Prepare the build environment while discarding its internal state object."""
    from fplinux_cli.environment.setup import setup  # noqa: PLC0415 -- setup-only backend.

    if offline:
        setup(force=force, offline=True)
    else:
        setup(force=force)


def _nand_backup_action(
    target: str,
    output: Path,
    *,
    profile: str | None,
    build_type: str,
    phone_data: Path | None = None,
) -> None:
    """Save a complete NAND image through the selected running system."""
    from fplinux_cli.cli.nand import (  # noqa: PLC0415 -- backup-only backend.
        backup_target_nand,
    )

    backup_target_nand(
        target, output, profile=profile, build_type=build_type, phone_data=phone_data
    )


def command_action(
    args: argparse.Namespace,
    check_parser: argparse.ArgumentParser,
) -> Callable[[], None]:
    """Bind parsed command arguments to one deferred command invocation."""
    if args.command == "doctor":
        from fplinux_cli.environment.doctor import doctor  # noqa: PLC0415 -- selected backend.

        action = doctor
    elif args.command == "logs":
        from fplinux_cli.cli.logs import read_logs  # noqa: PLC0415 -- selected backend.

        action = partial(read_logs, args)
    elif args.command == "inspect":
        if args.inspect_kind == "bundle":
            from fplinux_cli.cli.inspect import (  # noqa: PLC0415 -- selected backend.
                inspect_bundle,
            )

            action = partial(
                inspect_bundle, args.target, profile=args.profile, build_type=args.build_type
            )
        elif args.inspect_kind == "archive":
            from fplinux_cli.cli.inspect import (  # noqa: PLC0415 -- selected backend.
                inspect_archive,
            )

            action = partial(inspect_archive, args.path)
        elif args.inspect_kind == "distribution":
            from fplinux_cli.cli.inspect import (  # noqa: PLC0415 -- selected backend.
                inspect_distribution,
            )

            action = partial(inspect_distribution, args.path)
        elif args.inspect_kind == "footprint":
            from fplinux_cli.cli.inspect import (  # noqa: PLC0415 -- selected backend.
                inspect_target_footprint,
            )

            action = partial(
                inspect_target_footprint,
                args.target,
                profile=args.profile,
                build_type=args.build_type,
                json_output=args.json,
            )
        elif args.inspect_kind == "footprint-diff":
            from fplinux_cli.cli.inspect import (  # noqa: PLC0415 -- selected backend.
                inspect_footprint_diff,
            )

            action = partial(
                inspect_footprint_diff, args.before, args.after, json_output=args.json
            )
        else:
            from fplinux_cli.cli.inspect import inspect_apk  # noqa: PLC0415 -- selected backend.

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
            from fplinux_cli.quality.checks import check  # noqa: PLC0415 -- selected backend.

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
            from fplinux_cli.dependencies.workflow import (  # noqa: PLC0415 -- selected backend.
                create_dependencies,
            )

            action = partial(
                create_dependencies,
                args.directory,
                offline=args.offline,
                sources=args.sources,
                inputs_only=args.inputs_only,
            )
        elif args.dependencies_command == "verify":
            from fplinux_cli.dependencies.workflow import (  # noqa: PLC0415 -- selected backend.
                verify_dependencies,
            )

            action = partial(verify_dependencies, args.directory)
        elif args.dependencies_command == "restore":
            from fplinux_cli.dependencies.workflow import (  # noqa: PLC0415 -- selected backend.
                restore_dependencies,
            )

            if args.release is not None:
                if args.directory is not None or args.inputs_only:
                    fail("restore --release cannot be combined with a directory or --inputs-only")
                from fplinux_cli.dependencies.releases import (  # noqa: PLC0415 -- public release download.
                    restore_release,
                )

                action = partial(restore_release, args.release)
            else:
                if args.directory is None:
                    fail("restore requires a snapshot directory or --release ID")
                action = partial(
                    restore_dependencies, args.directory, inputs_only=args.inputs_only
                )
        elif args.dependencies_command == "export":
            from fplinux_cli.dependencies.workflow import (  # noqa: PLC0415 -- selected backend.
                export_dependencies,
            )

            action = partial(export_dependencies, args.directory, args.output)
        else:
            from fplinux_cli.dependencies.workflow import (  # noqa: PLC0415 -- selected backend.
                import_dependencies,
            )

            action = partial(import_dependencies, args.directory, args.output)
    elif args.command == "ci":
        from fplinux_cli.ci.commands import run_ci  # noqa: PLC0415 -- selected backend.

        action = partial(run_ci, args)
    elif args.command == "test":
        from fplinux_cli.quality.testing import run_tests  # noqa: PLC0415 -- selected backend.

        action = partial(
            run_tests, args.names, tier=args.tier, verbose=args.verbose, failfast=args.failfast
        )
    elif args.command == "format":
        from fplinux_cli.quality.formatting.command import (  # noqa: PLC0415 -- selected backend.
            format_sources,
        )

        action = partial(format_sources, args.paths)
    elif args.command == "_commit-msg":
        from fplinux_cli.quality.git import (  # noqa: PLC0415 -- selected backend.
            check_commit_message,
        )

        action = partial(check_commit_message, args.message_file)
    elif args.command == "_pre-push":
        from fplinux_cli.quality.pre_push import (  # noqa: PLC0415 -- selected backend.
            check_pre_push,
        )

        action = check_pre_push
    elif args.command == "build":
        from fplinux_cli.build.workflow import build  # noqa: PLC0415 -- selected backend.

        action = partial(
            build,
            args.target,
            args.jobs,
            memory_mib=args.memory_mib,
            profile=args.profile,
            build_type=args.build_type,
            include_device_data=args.include_device_data,
            verbose=args.verbose,
            offline=args.offline,
        )
    elif args.command == "checksum":
        from fplinux_cli.alpine.checksum import (  # noqa: PLC0415 -- selected backend.
            checksum_aport,
        )

        action = partial(checksum_aport, args.aport, offline=args.offline)
    elif args.command == "probe-build":
        from fplinux_cli.build.probe import build_probe  # noqa: PLC0415 -- selected backend.

        action = partial(build_probe, args.source, output=args.output)
    elif args.command == "package":
        from fplinux_cli.packaging.archive import (  # noqa: PLC0415 -- selected backend.
            package_target,
        )

        action = partial(
            package_target,
            args.target,
            profile=args.profile,
            boot=args.boot,
            host=args.host,
            output_directory=args.output_directory,
        )
    elif args.command == "prune":
        from fplinux_cli.cache.prune.operations import prune  # noqa: PLC0415 -- selected backend.

        action = partial(prune, apply=args.prune_apply)
    elif args.command == "run":
        from fplinux_cli.runtime.runner import run_target  # noqa: PLC0415 -- selected backend.

        action = partial(
            run_target,
            args.target,
            profile=args.profile,
            build_type=args.build_type,
            boot=args.boot,
            events=args.events,
        )
        if args.loader_trace:
            action = partial(action, loader_trace=True)
        if args.from_dump is not None:
            action = partial(action, from_dump=args.from_dump)
    elif args.command == "console":
        from fplinux_cli.cli.runtime import console_target  # noqa: PLC0415 -- selected backend.

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
            from fplinux_cli.cli.nand import (  # noqa: PLC0415 -- selected backend.
                identify_target_nand,
            )

            action = partial(
                identify_target_nand, args.target, profile=args.profile, build_type=args.build_type
            )
        else:
            raise AssertionError(f"unhandled nand command: {args.nand_command}")
    elif args.command == "device-data":
        from fplinux_cli.cli.device_data import (  # noqa: PLC0415 -- selected backend.
            prepare_device_data,
            report_device_data,
        )

        if args.device_data_command == "prepare":
            action = partial(
                prepare_device_data,
                args.target,
                from_dump=args.from_dump,
                from_originals=args.from_originals,
                events=args.events,
                jobs=args.jobs,
                offline=args.offline,
            )
        elif args.device_data_command == "report":
            action = partial(
                report_device_data,
                args.target,
                from_dump=args.from_dump,
                offline=args.offline,
            )
        else:
            raise AssertionError(f"unhandled device-data command: {args.device_data_command}")
    elif args.command == "target":
        if args.target_command != "new":
            raise AssertionError(f"unhandled target command: {args.target_command}")
        from fplinux_cli.cli.target_new import create_target  # noqa: PLC0415 -- selected backend.

        action = partial(
            create_target,
            args.name,
            platform=args.platform,
            brand=args.brand,
            product=args.product,
            compatible=args.compatible,
        )
    elif args.command == "verify":
        from fplinux_cli.cli.runtime import verify_booted  # noqa: PLC0415 -- selected backend.

        action = partial(
            verify_booted, args.target, profile=args.profile, build_type=args.build_type
        )
    else:
        raise AssertionError(f"unhandled command: {args.command}")
    phone_data = getattr(args, "phone_data", None)
    if phone_data is not None:
        # This selector is exposed only by parsers whose backends accept it.
        action = partial(cast("Callable[..., None]", action), phone_data=phone_data)
    return action

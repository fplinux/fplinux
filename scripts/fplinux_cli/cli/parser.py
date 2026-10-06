# SPDX-License-Identifier: GPL-2.0-only
"""Argument declarations for the repository-local FPLinux commands."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from fplinux_cli.cli.logs import add_log_arguments
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.paths import GLOBAL_PROFILES
from fplinux_cli.manifests.values import TARGET_NAME
from fplinux_cli.quality.scopes import CHECK_SCOPES
from fplinux_cli.quality.testing import add_test_arguments
from fplinux_cli.runtime.bundle_session import PUBLIC_BOOT_MODES

_CHECK_SCOPE_METAVAR = "{" + ",".join(CHECK_SCOPES) + "}"
_PUBLIC_COMMAND_METAVAR = (
    "{doctor,check,test,logs,inspect,format,setup,dependencies,build,probe-build,checksum,package,prune,"
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


def _default_build_jobs() -> int:
    """Respect available CPUs while leaving a busy workstation usable."""
    return min(8, os.process_cpu_count() or 1)


def _profile_name(value: str) -> str:
    """Accept one of the two public build profiles."""
    if TARGET_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError(f"invalid profile name: {value!r}")
    if value not in GLOBAL_PROFILES:
        raise argparse.ArgumentTypeError(
            f"unknown profile: {value!r} (choose from default, microsd-uboot)"
        )
    return value


def _add_build_type_option(parser: argparse.ArgumentParser) -> None:
    """Use the same explicit kernel type selector for every bundle consumer."""
    parser.add_argument(
        "--build-type",
        choices=BUILD_TYPES,
        default="release",
        help="select the kernel build type (default: release)",
    )


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
        "--events", type=Path, metavar="PATH", help="write loader events as JSON lines"
    )
    prepare.add_argument(
        "--from-dump",
        type=Path,
        metavar="PATH",
        help="use an existing physical NAND backup without connecting the phone",
    )
    prepare.add_argument(
        "--jobs",
        type=_positive_jobs,
        default=_default_build_jobs(),
        metavar="N",
        help="limit jobs when the read-only NAND loader must be built",
    )
    prepare.add_argument(
        "--offline",
        action="store_true",
        help="build the read-only NAND loader without network access",
    )


def create_parser(
    targets: tuple[str, ...],
) -> tuple[argparse.ArgumentParser, argparse.ArgumentParser]:
    """Declare command arguments and retain check-specific validation output."""
    parser = argparse.ArgumentParser(prog="fplinux")
    commands = parser.add_subparsers(
        dest="command",
        required=True,
        metavar=_PUBLIC_COMMAND_METAVAR,
    )
    commands.add_parser("doctor", help="check the project-local build runtime")
    check_parser = commands.add_parser("check", help="run the source quality gate")
    _add_build_type_option(check_parser)
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
    _add_build_type_option(bundle_parser)
    bundle_parser.add_argument("target", choices=targets)
    bundle_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    footprint_parser = inspections.add_parser(
        "footprint", help="measure the current bundle's size and package composition"
    )
    _add_build_type_option(footprint_parser)
    footprint_parser.add_argument("target", choices=targets)
    footprint_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    footprint_parser.add_argument("--json", action="store_true", help="print the full size report")
    diff_parser = inspections.add_parser(
        "footprint-diff", help="compare two saved footprint JSON reports"
    )
    diff_parser.add_argument("before", type=Path, metavar="BEFORE.json")
    diff_parser.add_argument("after", type=Path, metavar="AFTER.json")
    diff_parser.add_argument("--json", action="store_true", help="print the full comparison")
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
        "--offline",
        action="store_true",
        help="recreate the pinned environment from exact local inputs without network access",
    )
    setup_parser.add_argument(
        "--force",
        action="store_true",
        help="rebuild the pinned image even when the current recipe is ready",
    )
    dependencies_parser = commands.add_parser(
        "dependencies", help="preserve exact external build inputs"
    )
    dependency_commands = dependencies_parser.add_subparsers(
        dest="dependencies_command", required=True, metavar="{create,verify,restore}"
    )
    create_snapshot_parser = dependency_commands.add_parser(
        "create", help="save a dependency snapshot outside .cache"
    )
    create_snapshot_parser.add_argument("directory", type=Path, metavar="DIRECTORY")
    create_snapshot_parser.add_argument(
        "--offline", action="store_true", help="require every exact input to be stored locally"
    )
    create_snapshot_parser.add_argument(
        "--from",
        dest="sources",
        type=Path,
        action="append",
        default=[],
        metavar="PATH",
        help="find exact originals in an additional local file or directory",
    )
    create_snapshot_parser.add_argument(
        "--inputs-only",
        action="store_true",
        help="save external inputs without exporting the current Kern image",
    )
    verify_snapshot_parser = dependency_commands.add_parser(
        "verify", help="verify a snapshot's bytes and current declarations"
    )
    verify_snapshot_parser.add_argument("directory", type=Path, metavar="DIRECTORY")
    restore_snapshot_parser = dependency_commands.add_parser(
        "restore", help="restore exact inputs and the saved Kern image without downloading"
    )
    restore_snapshot_parser.add_argument("directory", type=Path, metavar="DIRECTORY")
    restore_snapshot_parser.add_argument(
        "--inputs-only",
        action="store_true",
        help="restore inputs for a fresh offline environment build",
    )
    commit_message_parser = commands.add_parser("_commit-msg")
    commit_message_parser.add_argument("message_file")
    build_parser = commands.add_parser("build", help="build a target in .cache/out")
    _add_build_type_option(build_parser)
    build_parser.add_argument("target", choices=targets)
    build_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="build one global profile (default: default)",
    )
    build_parser.add_argument(
        "--jobs",
        type=int,
        default=_default_build_jobs(),
        help="limit parallel compilation (default: available CPUs, up to 8)",
    )
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
    _add_build_type_option(package_parser)
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
    _add_build_type_option(run_parser)
    run_parser.add_argument("target", choices=targets)
    run_parser.add_argument(
        "--events", type=Path, metavar="PATH", help="write loader events as JSON lines"
    )
    _add_boot_profile_options(run_parser, "run")

    console_parser = commands.add_parser("console", help="connect to a running target over USB")
    _add_build_type_option(console_parser)
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
    _add_build_type_option(nand_backup_parser)
    nand_backup_parser.add_argument("target", choices=targets)
    nand_backup_parser.add_argument("output", type=Path)
    nand_backup_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    nand_identify_parser = nand_commands.add_parser(
        "identify", help="print the NAND chip identity and geometry reported by the phone"
    )
    _add_build_type_option(nand_identify_parser)
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
    _add_build_type_option(verify_parser)
    verify_parser.add_argument("target", choices=targets)
    verify_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    return parser, check_parser

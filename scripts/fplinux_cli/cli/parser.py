# SPDX-License-Identifier: GPL-2.0-only
"""Argument declarations for the repository-local FPLinux commands."""

from __future__ import annotations

import argparse
import os
from pathlib import Path

from fplinux_cli.build import DEFAULT_BUILD_MEMORY_MIB
from fplinux_cli.cli.logs import add_log_arguments
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.paths import GLOBAL_PROFILES
from fplinux_cli.manifests.values import TARGET_NAME
from fplinux_cli.quality.scopes import CHECK_SCOPES
from fplinux_cli.quality.testing import add_test_arguments
from fplinux_cli.runtime.bundle_session import PUBLIC_BOOT_MODES

_CHECK_SCOPE_METAVAR = "{" + ",".join(CHECK_SCOPES) + "}"
_PUBLIC_COMMAND_METAVAR = (
    "{doctor,check,test,logs,inspect,format,setup,dependencies,ci,build,probe-build,checksum,package,prune,"
    "run,console,nand,device-data,target,verify}"
)


def _check_scope(value: str) -> str:
    """Validate one optional check scope without treating an empty list as a value."""
    if value not in CHECK_SCOPES:
        choices = ", ".join(repr(scope) for scope in CHECK_SCOPES)
        raise argparse.ArgumentTypeError(f"invalid choice: {value!r} (choose from {choices})")
    return value


def _positive_integer(value: str) -> int:
    """Parse one positive resource limit before it can take the cache lock."""
    try:
        number = int(value)
    except ValueError as error:
        message = "must be a positive integer"
        raise argparse.ArgumentTypeError(message) from error
    if number < 1:
        message = "must be a positive integer"
        raise argparse.ArgumentTypeError(message)
    return number


def _default_build_jobs() -> int:
    """Respect available CPUs while leaving a busy workstation usable."""
    return min(8, os.process_cpu_count() or 1)


def _profile_name(value: str) -> str:
    """Accept one declared public build profile."""
    if TARGET_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError(f"invalid profile name: {value!r}")
    if value not in GLOBAL_PROFILES:
        raise argparse.ArgumentTypeError(
            f"unknown profile: {value!r} (choose from {', '.join(GLOBAL_PROFILES)})"
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


def _add_phone_data_option(parser: argparse.ArgumentParser) -> None:
    """Select a separate persistent store for another phone of the same target."""
    parser.add_argument(
        "--phone-data",
        type=Path,
        metavar="PATH",
        help="use this phone data directory instead of the user's shared target store",
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
        help=f"{verb} this build profile",
    )


def _add_device_data_commands(
    parser: argparse.ArgumentParser,
    targets: tuple[str, ...],
) -> None:
    """Declare normal phone-data preparation and the separate board report."""
    commands = parser.add_subparsers(
        dest="device_data_command",
        required=True,
        metavar="{prepare,report}",
    )
    prepare = commands.add_parser(
        "prepare",
        help="prepare all declared device data from one supported source",
    )
    prepare.add_argument("target", choices=targets)
    _add_phone_data_option(prepare)
    source = prepare.add_mutually_exclusive_group()
    source.add_argument(
        "--events", type=Path, metavar="PATH", help="write loader events as JSON lines"
    )
    source.add_argument(
        "--from-dump",
        type=Path,
        metavar="PATH",
        help="use a NAND or eMMC user-area backup without connecting the phone",
    )
    source.add_argument(
        "--from-originals",
        type=Path,
        metavar="MANIFEST.json",
        help="use a manifest of named original files without connecting the phone",
    )
    prepare.add_argument(
        "--jobs",
        type=_positive_integer,
        default=_default_build_jobs(),
        metavar="N",
        help="limit jobs when the read-only phone loader must be built",
    )
    prepare.add_argument(
        "--offline",
        action="store_true",
        help="build the read-only phone loader without network access",
    )
    report = commands.add_parser(
        "report", help="extract a board report from a saved NAND without selecting phone data"
    )
    report.add_argument("target", choices=targets)
    report.add_argument("--from-dump", type=Path, required=True, metavar="PATH")
    report.add_argument(
        "--offline", action="store_true", help="prepare the tool without network access"
    )


def _add_ci_commands(commands: argparse._SubParsersAction[argparse.ArgumentParser]) -> None:
    """Expose the release workflow's locally reproducible stages."""
    ci_parser = commands.add_parser("ci", help="prepare and verify complete releases")
    stages = ci_parser.add_subparsers(dest="ci_command", required=True)
    stages.add_parser("matrix", help="print the supported runtime archive selections")
    stages.add_parser("bootstrap", help="check native build-runner prerequisites")
    stages.add_parser("site-install", help="install documentation tooling from saved originals")
    prepare = stages.add_parser(
        "prepare-inputs", help="preserve exact inputs for all release jobs"
    )
    prepare.add_argument("--output-dir", type=Path, required=True, metavar="PATH")
    restore = stages.add_parser("restore-inputs", help="restore this run's preserved input set")
    restore.add_argument("--directory", type=Path, required=True, metavar="PATH")
    restore.add_argument("--snapshot", required=True, metavar="SHA256")
    preflight = stages.add_parser("preflight", help="install exact inputs in a fresh checkout")
    preflight.add_argument("--commit", required=True, metavar="SHA")
    preflight.add_argument("--directory", type=Path, required=True, metavar="PATH")
    assemble = stages.add_parser("assemble", help="assemble the complete runtime and source set")
    assemble.add_argument("--input-dir", type=Path, required=True, metavar="PATH")
    assemble.add_argument("--output-dir", type=Path, required=True, metavar="PATH")
    assemble.add_argument("--inputs-dir", type=Path, required=True, metavar="PATH")
    smoke = stages.add_parser("smoke", help="start and reopen archives on their native host")
    smoke.add_argument("--directory", type=Path, required=True, metavar="PATH")
    smoke.add_argument("--host", choices=("linux", "windows"), required=True)
    publish = stages.add_parser("publish", help="publish a verified complete release")
    publish.add_argument("--directory", type=Path, required=True, metavar="PATH")
    publish.add_argument("--commit", required=True, metavar="SHA")


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
    check_parser = commands.add_parser(
        "check",
        help="run quality checks and tests",
        description=(
            "Run every quality scope by default. The python scope runs Python analyzers; "
            "the tests scope runs all five test tiers."
        ),
    )
    _add_build_type_option(check_parser)
    check_parser.add_argument(
        "scopes",
        nargs="*",
        type=_check_scope,
        metavar=_CHECK_SCOPE_METAVAR,
        help="select scopes; omit to run all checks and tests",
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
        help="stream complete stage output; incompatible with --jobs greater than 1",
    )
    check_parser.add_argument(
        "--no-cache",
        action="store_true",
        help="rerun selected checks even when unchanged inputs already passed",
    )
    check_parser.add_argument(
        "--jobs",
        type=_positive_integer,
        default=None,
        metavar="N",
        help=(
            "limit concurrent kernel check contexts "
            "(default: 3 with kernel, 1 without kernel or with --verbose)"
        ),
    )
    check_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="check this build profile (default: default)",
    )
    test_parser = commands.add_parser(
        "test", help="run selected pytest tests in the pinned environment"
    )
    add_test_arguments(test_parser)
    logs_parser = commands.add_parser("logs", help="list, inspect or follow recorded command logs")
    add_log_arguments(logs_parser)
    inspect_parser = commands.add_parser("inspect", help="inspect a built bundle, ZIP or APK")
    inspections = inspect_parser.add_subparsers(dest="inspect_kind", required=True)
    bundle_parser = inspections.add_parser("bundle", help="inspect the current target bundle")
    _add_build_type_option(bundle_parser)
    bundle_parser.add_argument("target", choices=targets)
    bundle_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="inspect this build profile (default: default)",
    )
    footprint_parser = inspections.add_parser(
        "footprint", help="measure the current bundle's size and package composition"
    )
    _add_build_type_option(footprint_parser)
    footprint_parser.add_argument("target", choices=targets)
    footprint_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="measure this build profile (default: default)",
    )
    footprint_parser.add_argument("--json", action="store_true", help="print the full size report")
    diff_parser = inspections.add_parser(
        "footprint-diff", help="compare two saved footprint JSON reports"
    )
    diff_parser.add_argument("before", type=Path, metavar="BEFORE.json")
    diff_parser.add_argument("after", type=Path, metavar="AFTER.json")
    diff_parser.add_argument("--json", action="store_true", help="print the full comparison")
    for kind, help_text in (
        ("archive", "check and inspect a FPLinux ZIP"),
        ("distribution", "verify a complete release directory"),
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
    setup_parser = commands.add_parser("setup", help="prepare the pinned build environment")
    setup_parser.add_argument(
        "--offline",
        action="store_true",
        help="recreate the pinned environment from exact local inputs without network access",
    )
    setup_parser.add_argument(
        "--force",
        action="store_true",
        help="rebuild the pinned build environment even when it is ready",
    )
    dependencies_parser = commands.add_parser(
        "dependencies", help="preserve exact external build inputs"
    )
    dependency_commands = dependencies_parser.add_subparsers(
        dest="dependencies_command", required=True, metavar="{create,verify,restore,export,import}"
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
        help="save external inputs without archiving the current build environment",
    )
    verify_snapshot_parser = dependency_commands.add_parser(
        "verify", help="verify a snapshot's bytes and current declarations"
    )
    verify_snapshot_parser.add_argument("directory", type=Path, metavar="DIRECTORY")
    restore_snapshot_parser = dependency_commands.add_parser(
        "restore", help="restore dependencies from a local snapshot or download release inputs"
    )
    restore_snapshot_parser.add_argument(
        "directory",
        type=Path,
        nargs="?",
        metavar="DIRECTORY",
        help="local snapshot containing inputs and, when saved, the build environment",
    )
    restore_snapshot_parser.add_argument(
        "--release",
        metavar="ID",
        help="download this release's dependency inputs; prepare its build environment separately",
    )
    restore_snapshot_parser.add_argument(
        "--inputs-only",
        action="store_true",
        help="restore inputs for a fresh offline environment build",
    )
    export_parser = dependency_commands.add_parser(
        "export", help="split an inputs-only snapshot into checked transport parts"
    )
    export_parser.add_argument("directory", type=Path, metavar="SNAPSHOT")
    export_parser.add_argument("output", type=Path, metavar="OUTPUT")
    import_parser = dependency_commands.add_parser(
        "import", help="verify transport parts and extract a dependency snapshot"
    )
    import_parser.add_argument("directory", type=Path, metavar="PARTS")
    import_parser.add_argument("output", type=Path, metavar="OUTPUT")
    _add_ci_commands(commands)
    commit_message_parser = commands.add_parser("_commit-msg")
    commit_message_parser.add_argument("message_file")
    commands.add_parser("_pre-push")
    build_parser = commands.add_parser("build", help="build a target in .cache/out")
    _add_build_type_option(build_parser)
    _add_phone_data_option(build_parser)
    build_parser.add_argument("target", choices=targets)
    build_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="build this profile (default: default)",
    )
    build_parser.add_argument(
        "--without-device-data",
        dest="include_device_data",
        action="store_false",
        help="exclude locally prepared device data from the build",
    )
    build_parser.add_argument(
        "--jobs",
        type=int,
        default=_default_build_jobs(),
        help="limit parallel compilation (default: available CPUs, up to 8)",
    )
    build_parser.add_argument(
        "--memory-mib",
        type=_positive_integer,
        default=DEFAULT_BUILD_MEMORY_MIB,
        metavar="N",
        help="set the build memory budget in MiB (default: %(default)s)",
    )
    build_parser.add_argument(
        "--verbose",
        action="store_true",
        help="stream complete stage output while retaining logs",
    )
    build_parser.add_argument(
        "--offline",
        action="store_true",
        help="build without downloads, using the prepared environment and saved inputs",
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
        help="regenerate one Alpine aport sha512sums block in the pinned build environment",
    )
    checksum_parser.add_argument("aport")
    checksum_parser.add_argument(
        "--offline",
        action="store_true",
        help="regenerate using only exact originals already stored locally",
    )
    package_parser = commands.add_parser(
        "package", help="package an existing build for standalone use"
    )
    package_parser.add_argument("target", choices=targets)
    _add_boot_profile_options(package_parser, "package")
    package_parser.add_argument(
        "--host",
        choices=("linux", "windows"),
        default="linux",
        help="select the x86-64 host (default: linux)",
    )
    package_parser.add_argument(
        "--output-directory", type=Path, metavar="PATH", help="write both ZIPs to this directory"
    )
    prune_parser = commands.add_parser(
        "prune", help="list disposable cached files, or remove them with --apply"
    )
    prune_parser.add_argument(
        "--apply",
        dest="prune_apply",
        action="store_true",
        help="remove disposable staged workspaces",
    )
    run_parser = commands.add_parser(
        "run", help="prepare phone data, start Linux or reopen its SSH connection"
    )
    _add_build_type_option(run_parser)
    _add_phone_data_option(run_parser)
    run_parser.add_argument("target", choices=targets)
    run_parser.add_argument(
        "--from-dump",
        type=Path,
        metavar="PATH",
        help="prepare the first run from an existing NAND or eMMC user-area backup",
    )
    run_parser.add_argument(
        "--events", type=Path, metavar="PATH", help="write loader events as JSON lines"
    )
    run_parser.add_argument(
        "--loader-trace",
        action="store_true",
        help="report RAM-loader protocol commands and reply sizes without payload bytes",
    )
    _add_boot_profile_options(run_parser, "run")

    console_parser = commands.add_parser("console", help="connect to a running target over USB")
    _add_build_type_option(console_parser)
    _add_phone_data_option(console_parser)
    console_parser.add_argument("target", choices=targets)
    console_parser.add_argument(
        "--profile",
        type=_profile_name,
        metavar="NAME",
        help="reconnect to a session started with this build profile",
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
    _add_phone_data_option(nand_backup_parser)
    nand_backup_parser.add_argument("target", choices=targets)
    nand_backup_parser.add_argument("output", type=Path)
    nand_backup_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    nand_identify_parser = nand_commands.add_parser(
        "identify", help="print the NAND chip identity and geometry reported by the phone"
    )
    _add_build_type_option(nand_identify_parser)
    _add_phone_data_option(nand_identify_parser)
    nand_identify_parser.add_argument("target", choices=targets)
    nand_identify_parser.add_argument("--profile", type=_profile_name, metavar="NAME")

    device_data_parser = commands.add_parser(
        "device-data", help="prepare phone data or inspect a saved NAND for a board report"
    )
    _add_device_data_commands(device_data_parser, targets)

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
    _add_phone_data_option(verify_parser)
    verify_parser.add_argument("target", choices=targets)
    verify_parser.add_argument("--profile", type=_profile_name, metavar="NAME")
    return parser, check_parser

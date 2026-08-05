# SPDX-License-Identifier: GPL-2.0-only
"""Command-line parser for the repository-local FPLinux interface."""

from __future__ import annotations

import argparse
import os

from .commands import build, package_target, run_target
from .config import discover_targets
from .container import check, doctor, setup


def main() -> None:
    targets = discover_targets()
    parser = argparse.ArgumentParser(prog="fplinux")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor", help="check the rootless build host")
    commands.add_parser("check", help="run the source quality gate")
    setup_parser = commands.add_parser("setup", help="build the pinned OCI environment")
    setup_parser.add_argument("--force", action="store_true")
    build_parser = commands.add_parser("build", help="build a target in .cache/out")
    build_parser.add_argument("target", choices=targets)
    build_parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 1))
    package_parser = commands.add_parser(
        "package", help="package an existing build for Linux x86-64"
    )
    package_parser.add_argument("target", choices=targets)
    package_parser.add_argument(
        "--candidate",
        action="store_true",
        help="create a clearly marked hardware-qualification candidate",
    )
    run_parser = commands.add_parser("run", help="run a target's volatile-RAM loader")
    run_parser.add_argument("target", choices=targets)
    args = parser.parse_args()

    if args.command == "doctor":
        doctor()
    elif args.command == "check":
        check()
    elif args.command == "setup":
        setup(force=args.force)
    elif args.command == "build":
        build(args.target, args.jobs)
    elif args.command == "package":
        package_target(args.target, candidate=args.candidate)
    elif args.command == "run":
        run_target(args.target)


if __name__ == "__main__":
    main()

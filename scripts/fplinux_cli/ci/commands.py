# SPDX-License-Identifier: GPL-2.0-only
"""Connect trusted CI operations to the public command parser."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import argparse


def run_ci(args: argparse.Namespace) -> None:
    """Keep runner orchestration in the same executable interface as local builds."""
    if args.ci_command == "bootstrap":
        from .bootstrap import bootstrap_host  # noqa: PLC0415 -- Linux-only backend.

        bootstrap_host()
    elif args.ci_command == "prepare-inputs":
        from .inputs import prepare_inputs  # noqa: PLC0415 -- selected backend.

        prepare_inputs(args.output_dir)
    elif args.ci_command == "restore-inputs":
        from .inputs import restore_inputs  # noqa: PLC0415 -- selected backend.

        restore_inputs(args.directory, snapshot=args.snapshot)
    elif args.ci_command == "preflight":
        from .preflight import prepare_push  # noqa: PLC0415 -- explicit preparation.

        prepare_push(args.commit, args.directory)
    elif args.ci_command == "site-install":
        from fplinux_cli.dependencies.site_install import install_site  # noqa: PLC0415

        install_site()
    elif args.ci_command == "attach-inputs":
        from fplinux_cli.packaging.distribution import (  # noqa: PLC0415
            attach_inputs,
            distribution_matrix,
        )

        attach_inputs(
            args.directory,
            args.inputs_dir,
            args.output_directory,
            expected_matrix=distribution_matrix(),
        )
    elif args.ci_command == "smoke":
        from .smoke import smoke_distribution  # noqa: PLC0415 -- native runtime backend.

        smoke_distribution(args.directory, host=args.host)
    elif args.ci_command == "publish":
        from .release import publish_distribution  # noqa: PLC0415 -- selected backend.

        publish_distribution(args.directory, commit=args.commit)
    else:
        raise AssertionError(f"unhandled CI command: {args.ci_command}")

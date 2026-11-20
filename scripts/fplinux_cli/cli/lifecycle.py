# SPDX-License-Identifier: GPL-2.0-only
"""Execute commands under their cache ownership and retention policy."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.cache.lock import cache_lock
from fplinux_cli.common import ROOT
from fplinux_cli.data_paths import data_root
from fplinux_cli.runtime.bundle_session import selected_context_profile
from fplinux_cli.source_paths import validate_checkout_data_root

if TYPE_CHECKING:
    import argparse
    from collections.abc import Callable
    from pathlib import Path


_EXCLUSIVE_CACHE_COMMANDS = frozenset(
    {
        "build",
        "card",
        "check",
        "checksum",
        "dependencies",
        "device-data",
        "format",
        "package",
        "probe-build",
        "setup",
        "site",
        "test",
    }
)

_SHARED_CACHE_COMMANDS = frozenset({"console", "nand", "run", "verify"})


def _cache_lock_exclusive(args: argparse.Namespace) -> bool | None:
    """Return the command-wide cache-lock mode, if this invocation needs one."""
    if (args.command == "check" and args.list_scopes) or (args.command == "format" and args.plan):
        return None
    if args.command == "prune":
        return True if args.prune_apply else None
    if args.command == "inspect":
        return False if args.inspect_kind in {"bundle", "footprint"} else None
    if args.command in _EXCLUSIVE_CACHE_COMMANDS or (
        args.command == "ci"
        and args.ci_command in {"prepare-inputs", "restore-inputs", "site-install", "preflight"}
    ):
        return True
    if args.command in _SHARED_CACHE_COMMANDS:
        return False
    return None


def execute_command(
    args: argparse.Namespace, action: Callable[[], object], *, data_directory: Path | None = None
) -> None:
    """Run exactly one dispatch action while its command-wide cache lock is held."""
    exclusive = _cache_lock_exclusive(args)
    selected_root = data_root(ROOT) if data_directory is None else data_directory
    if exclusive is not None:
        validate_checkout_data_root(ROOT, selected_root)
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
        selected_root,
        exclusive=exclusive,
        command=args.command,
        target=target if isinstance(target, str) else None,
        profile=profile,
        timeout=getattr(args, "lock_timeout", None),
    ):
        try:
            action()
        finally:
            if args.command == "check" and profile is not None:
                from fplinux_cli.cache.prune.operations import (  # noqa: PLC0415 -- profile-only cleanup.
                    discard_superseded_profile_logs,
                )

                discard_superseded_profile_logs(selected_root, "check", profile=profile)

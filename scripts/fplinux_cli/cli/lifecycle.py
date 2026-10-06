# SPDX-License-Identifier: GPL-2.0-only
"""Execute commands under their cache ownership and retention policy."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.cache.lock import cache_lock
from fplinux_cli.cache.prune.operations import (
    discard_obsolete_apks,
    discard_obsolete_rootfs,
    discard_superseded_profile_logs,
)
from fplinux_cli.common import ROOT
from fplinux_cli.runtime.bundle_session import selected_context_profile

if TYPE_CHECKING:
    import argparse
    from collections.abc import Callable


_EXCLUSIVE_CACHE_COMMANDS = frozenset(
    {
        "build",
        "check",
        "checksum",
        "dependencies",
        "device-data",
        "format",
        "nand",
        "probe-build",
        "setup",
        "test",
    }
)

_SHARED_CACHE_COMMANDS = frozenset({"console", "package", "run", "verify"})


def _cache_lock_exclusive(args: argparse.Namespace) -> bool | None:
    """Return the command-wide cache-lock mode, if this invocation needs one."""
    if args.command == "check" and args.list_scopes:
        return None
    if args.command == "prune":
        return True if args.prune_apply else None
    if args.command == "inspect":
        return False if args.inspect_kind in {"bundle", "footprint"} else None
    if args.command in _EXCLUSIVE_CACHE_COMMANDS:
        return True
    if args.command in _SHARED_CACHE_COMMANDS:
        return False
    return None


def execute_command(args: argparse.Namespace, action: Callable[[], None]) -> None:
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

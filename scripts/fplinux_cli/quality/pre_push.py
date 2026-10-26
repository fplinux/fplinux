# SPDX-License-Identifier: GPL-2.0-only
"""Verify the incoming main commit's preserved build inputs before a push."""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.ci.preflight import require_preflight
from fplinux_cli.common import fail

if TYPE_CHECKING:
    from collections.abc import Iterable


def incoming_main_commits(lines: Iterable[str]) -> tuple[str, ...]:
    """Select actual non-deletion main OIDs from Git's pre-push protocol."""
    commits: list[str] = []
    for line in lines:
        fields = line.split()
        if len(fields) != 4:
            fail("invalid pre-push ref record")
        _local_ref, local_oid, remote_ref, _remote_oid = fields
        if remote_ref != "refs/heads/main" or set(local_oid) == {"0"}:
            continue
        if len(local_oid) not in {40, 64} or any(c not in "0123456789abcdef" for c in local_oid):
            fail("invalid incoming main object ID")
        if local_oid not in commits:
            commits.append(local_oid)
    return tuple(commits)


def check_pre_push() -> None:
    """Gate main only; unrelated refs keep their existing push behavior."""
    for commit in incoming_main_commits(sys.stdin):
        receipt = require_preflight(common.ROOT, commit)
        print(
            f"Prepared main {commit}; clean installation completed {receipt['completed_at']}",
            flush=True,
        )

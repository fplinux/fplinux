# SPDX-License-Identifier: GPL-2.0-only
"""Distinguish an interrupted SSH connection from normal exit or a new RAM boot."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from fplinux_cli.common import fail


@dataclass(frozen=True)
class RunningBuild:
    """Build facts read from the authenticated running system."""

    kernel_release: str
    build_id: str


def read_running_build(connection: tuple[Any, dict[str, Any]]) -> RunningBuild:
    """Read existing system metadata with a bounded SSH request."""
    transport, session = connection
    result = transport.run_remote(
        session,
        "uname -r; sed -n 's/^BUILD_ID=//p' /etc/os-release",
        capture_output=True,
        shared=True,
        timeout=min(session.get("wait_seconds", 10), 10),
    )
    if result.returncode:
        fail(f"Cannot read the running phone build (exit {result.returncode})")
    lines = result.stdout.splitlines()
    if not lines or not lines[0].strip():
        fail("Cannot read the running phone kernel release")
    build_id = lines[1].strip() if len(lines) > 1 and lines[1].strip() else "unknown"
    return RunningBuild(lines[0].strip(), build_id)


class ConnectionLostError(Exception):
    """Interactive SSH failed or its authenticated USB device disappeared."""


class SessionResumedError(Exception):
    """An existing Linux session returned while a new loader was still waiting."""

    def __init__(self, session: dict[str, Any]) -> None:
        """Retain the existing credentials for authentication after candidate cleanup."""
        super().__init__("The existing Linux session is available")
        self.session = session

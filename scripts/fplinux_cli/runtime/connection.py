# SPDX-License-Identifier: GPL-2.0-only
"""Distinguish an interrupted SSH connection from normal exit or a new RAM boot."""

from __future__ import annotations

from typing import Any


class ConnectionLostError(Exception):
    """Interactive SSH failed or its authenticated USB device disappeared."""


class SessionResumedError(Exception):
    """An existing Linux session returned while a new loader was still waiting."""

    def __init__(self, session: dict[str, Any]) -> None:
        """Retain the existing credentials for authentication after candidate cleanup."""
        super().__init__("The existing Linux session is available")
        self.session = session

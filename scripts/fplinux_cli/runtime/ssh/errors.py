# SPDX-License-Identifier: GPL-2.0-only
"""SSH failure policy."""

from __future__ import annotations

from typing import NoReturn


def fail(message: str) -> NoReturn:
    """Stop without falling back to a less-bound transport."""
    raise SystemExit(f"fplinux ssh: {message}")

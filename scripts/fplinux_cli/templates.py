# SPDX-License-Identifier: GPL-2.0-only
"""Substitute named values in project-owned text templates."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Mapping

_PLACEHOLDER = re.compile(r"@([A-Z][A-Z_]*)@")


def substitute_template(text: str, values: Mapping[str, str]) -> str:
    """Replace @NAME@ once, rejecting placeholders without a supplied value."""

    def replacement(match: re.Match[str]) -> str:
        return values[match.group(1)]

    return _PLACEHOLDER.sub(replacement, text)

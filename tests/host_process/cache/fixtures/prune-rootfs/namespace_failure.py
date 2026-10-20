# SPDX-License-Identifier: GPL-2.0-only
"""Represent an unavailable subordinate namespace without touching the candidate."""

from __future__ import annotations

import sys

print("subordinate namespace unavailable", file=sys.stderr)
raise SystemExit(7)

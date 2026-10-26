# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001 -- standalone CI entry point copied into the ordinary-user workspace.
"""Enter native archive smoke from the ordinary Windows account workspace."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "scripts"))

from fplinux_cli.ci.smoke import smoke_distribution

smoke_distribution(Path(__file__).parent / "distribution", host="windows")

# SPDX-License-Identifier: GPL-2.0-only
"""Build the early screen's pinned fonts and activity masks for host harnesses."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING

import fplinux_activity
import fplinux_ui_fonts

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


def generate(directory: Path, ui_host_inputs: Path, profile: str | None = None) -> Path:
    tokens = json.loads((ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text())
    fonts = directory / "status-fonts.c"
    fonts.write_text(fplinux_ui_fonts.status_source(tokens, ui_host_inputs / "fonts", profile))
    if profile is None:
        (directory / "fplinux-activity-masks.h").write_text(fplinux_activity.render(tokens))
    return fonts

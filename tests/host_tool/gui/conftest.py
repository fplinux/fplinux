# SPDX-License-Identifier: GPL-2.0-only
"""Screenshot conversion used by the graphical component tests."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest

import tests.host_tool.gui.support.rgb565_png

if TYPE_CHECKING:
    from types import ModuleType


@pytest.fixture
def rgb565_png() -> ModuleType:
    """Use the screenshot converter in the current test process."""
    return tests.host_tool.gui.support.rgb565_png

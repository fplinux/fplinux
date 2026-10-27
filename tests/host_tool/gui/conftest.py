# SPDX-License-Identifier: GPL-2.0-only
"""Screenshot conversion used by the graphical component tests."""

from __future__ import annotations

import importlib
from typing import TYPE_CHECKING

import pytest

from tests import ROOT

if TYPE_CHECKING:
    from types import ModuleType


@pytest.fixture
def rgb565_png(monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    """Use the existing screenshot converter in the current test process."""
    monkeypatch.syspath_prepend(str(ROOT / "site/scripts/screens"))
    return importlib.import_module("rgb565-png")

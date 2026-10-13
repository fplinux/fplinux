# SPDX-License-Identifier: GPL-2.0-only
"""Controlled pytest input with an undeclared marker."""

import os
from pathlib import Path

import pytest


@pytest.mark.fplinux_typo_marker
def test_case() -> None:
    Path(os.environ["FPLINUX_TEST_TRACE"]).write_text("unexpected execution\n")

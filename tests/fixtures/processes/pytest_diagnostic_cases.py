# SPDX-License-Identifier: GPL-2.0-only
"""Controlled pytest outcomes for reporting checks."""

import pytest


@pytest.mark.skip(reason="controlled skip explanation")
def test_skipped() -> None:
    pytest.fail("the skipped case must not execute")


def test_passing() -> None:
    """Supply one measured successful pytest call."""

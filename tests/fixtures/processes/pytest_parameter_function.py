# SPDX-License-Identifier: GPL-2.0-only
"""Record the native parameter input selected by a runner subprocess."""

import os
from pathlib import Path

import pytest


@pytest.mark.parametrize("value", ["first", "second"])
def test_value(value: str) -> None:
    """Make the selected parameter observable independently of the runner."""
    with Path(os.environ["FPLINUX_TEST_TRACE"]).open("a") as stream:
        stream.write(value + "\n")

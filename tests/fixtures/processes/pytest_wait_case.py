# SPDX-License-Identifier: GPL-2.0-only
"""Expose readiness before waiting for the test controller to interrupt pytest."""

import time
from pathlib import Path


def test_wait() -> None:
    """Keep the child alive after reporting that pytest reached this case."""
    Path("ready").touch()
    time.sleep(30)

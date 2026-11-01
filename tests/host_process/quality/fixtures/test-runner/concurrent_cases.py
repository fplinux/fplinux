# SPDX-License-Identifier: GPL-2.0-only
"""Meet another pytest worker at a file barrier."""

import os
import time
from pathlib import Path


def test_concurrent() -> None:
    root = Path(os.environ["FPLINUX_TEST_TRACE"]).parent
    name = Path(__file__).stem.removeprefix("test_")
    other = "second" if name == "first" else "first"
    (root / name).write_text(os.environ["PYTEST_XDIST_WORKER"])
    deadline = time.monotonic() + 5
    while not (root / other).exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    assert (root / other).exists()

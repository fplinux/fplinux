# SPDX-License-Identifier: GPL-2.0-only
"""Make shared fixture acquisition and release observable across selected modules."""

from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

if TYPE_CHECKING:
    from collections.abc import Iterator


@pytest.fixture(scope="session", autouse=True)
def shared_resource() -> Iterator[None]:
    """Record the lifetime independently of the runner's command construction."""
    trace = Path(os.environ["FPLINUX_TEST_TRACE"])
    with trace.open("a") as stream:
        stream.write("resource:prepare\n")
    yield
    with trace.open("a") as stream:
        stream.write("resource:release\n")

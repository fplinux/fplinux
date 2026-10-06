# SPDX-License-Identifier: GPL-2.0-only
"""Controlled pytest outcomes, copied into a temporary test package by runner tests."""

import os
from pathlib import Path

import pytest


@pytest.fixture(autouse=True)
def _trace_selection(request: pytest.FixtureRequest) -> None:
    """Append the selected case to the caller-owned trace."""
    assert request.cls is not None
    name = f"{request.module.__name__}.{request.cls.__name__}.{request.node.name}"
    runtime = os.environ.get("FPLINUX_TEST_RUNTIME")
    if runtime is not None:
        name = f"{runtime}:{name}"
    with Path(os.environ["FPLINUX_TEST_TRACE"]).open("a") as stream:
        stream.write(name + "\n")


class FailureCase:
    """A deliberate first failure distinguishes failfast from the normal runner."""

    def test_first(self) -> None:
        """Report a controlled failure before the second case."""
        if os.environ.get("FPLINUX_TEST_FAIL", "1") == "1":
            pytest.fail("intentional selection-fixture failure")

    def test_second(self) -> None:
        """Provide a distinguishable sibling for selection checks."""


class PassingCase:
    """Record exactly which tests the runner selects."""

    def test_first(self) -> None:
        """Complete without changing external state beyond the trace."""

    def test_second(self) -> None:
        """Provide a distinguishable sibling for selection checks."""


class EmptyCase:
    """Represent a class that contains no runnable tests."""

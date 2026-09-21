# SPDX-License-Identifier: GPL-2.0-only
"""Controlled unittest outcomes, copied into a temporary test package by runner tests."""

import os
import unittest
from pathlib import Path


class PassingCase(unittest.TestCase):
    """Record exactly which tests the runner selects."""

    def setUp(self) -> None:
        """Append a test identifier to the caller-owned trace."""
        with Path(os.environ["FPLINUX_TEST_TRACE"]).open("a") as stream:
            stream.write(self.id() + "\n")

    def test_first(self) -> None:
        """Complete without changing external state beyond the trace."""

    def test_second(self) -> None:
        """Provide a distinguishable sibling for selection checks."""


class FailureCase(PassingCase):
    """A deliberate first failure distinguishes failfast from the normal runner."""

    def test_first(self) -> None:
        """Report a controlled failure before the inherited second case."""
        if os.environ.get("FPLINUX_TEST_FAIL", "1") == "1":
            self.fail("intentional selection-fixture failure")


class EmptyCase(unittest.TestCase):
    """Represent a class that contains no runnable tests."""

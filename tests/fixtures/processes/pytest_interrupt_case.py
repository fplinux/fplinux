# SPDX-License-Identifier: GPL-2.0-only
"""Interrupt the real pytest child executing this copied test module."""

import os
import signal


def test_interrupt() -> None:
    """Deliver SIGINT to the pytest process, preserving its normal handling."""
    os.kill(os.getpid(), signal.SIGINT)

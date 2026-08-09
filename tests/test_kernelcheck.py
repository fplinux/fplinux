# SPDX-License-Identifier: GPL-2.0-only
"""Tests for captured kernel-check command status handling."""

from __future__ import annotations

import signal
import subprocess
import unittest
from unittest import mock

from fplinux_cli import kernelcheck


class KernelCheckTests(unittest.TestCase):
    """Preserve shell-visible statuses from captured policy commands."""

    def test_checkpatch_signal_uses_shell_status(self) -> None:
        """Convert a negative subprocess return code before raising."""
        result = subprocess.CompletedProcess(["checkpatch"], -signal.SIGTERM, "", "")
        with (
            mock.patch.object(kernelcheck, "capture_text", return_value=result),
            mock.patch.object(kernelcheck, "record_text"),
            self.assertRaises(SystemExit) as raised,
        ):
            kernelcheck.run_checkpatch(["checkpatch"])
        self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)

    def test_dtbs_signal_uses_shell_status(self) -> None:
        """Convert a signalled dtbs_check status before propagating it."""
        result = subprocess.CompletedProcess(["dtbs_check"], -signal.SIGKILL, "", "")
        with (
            mock.patch.object(kernelcheck, "capture_text", return_value=result),
            mock.patch.object(kernelcheck, "record_text"),
            self.assertRaises(SystemExit) as raised,
        ):
            kernelcheck.run_dtbs_check(["dtbs_check"], "test-target")
        self.assertEqual(raised.exception.code, 128 + signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()

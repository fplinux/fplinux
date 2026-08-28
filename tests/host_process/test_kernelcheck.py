# SPDX-License-Identifier: GPL-2.0-only
"""Host-process tests for kernel-check status propagation."""

from __future__ import annotations

import contextlib
import io
import signal
import sys
import unittest

from fplinux_cli import kernelcheck


class KernelCheckSubprocessStatusTests(unittest.TestCase):
    """Preserve shell-visible statuses from isolated helper processes."""

    def test_checkpatch_signal_uses_shell_status(self) -> None:
        """Convert a helper SIGTERM return code before raising."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGTERM)",
        ]
        with contextlib.redirect_stdout(terminal), self.assertRaises(SystemExit) as raised:
            kernelcheck.run_checkpatch(command)
        self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)
        self.assertIn(f"checkpatch exited {-signal.SIGTERM}\n", terminal.getvalue())

    def test_dtbs_signal_uses_shell_status(self) -> None:
        """Convert a helper SIGKILL return code before propagating it."""
        terminal = io.StringIO()
        command = [
            sys.executable,
            "-c",
            "import os, signal; os.kill(os.getpid(), signal.SIGKILL)",
        ]
        with contextlib.redirect_stdout(terminal), self.assertRaises(SystemExit) as raised:
            kernelcheck.run_dtbs_check(command, "test-target")
        self.assertEqual(raised.exception.code, 128 + signal.SIGKILL)
        self.assertIn(
            f"dtbs_check exited {-signal.SIGKILL}: test-target\n",
            terminal.getvalue(),
        )


if __name__ == "__main__":
    unittest.main()

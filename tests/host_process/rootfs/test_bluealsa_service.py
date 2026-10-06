# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the BlueALSA pre-start hook with a fake external D-Bus endpoint."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from tests import ROOT

SERVICE = ROOT / "alpine/aports/fplinux-bluealsa/bluealsa.initd"
ENDPOINT = ROOT / "tests/fixtures/processes/dbus_reload_endpoint.py"


class BluealsaServiceHookTests(unittest.TestCase):
    """Observe hook results; the fake does not establish real bus or daemon behavior."""

    def run_hook(
        self, directory: Path, *, reject_reload: bool
    ) -> subprocess.CompletedProcess[str]:
        """Source the complete init script and replace only the external sender."""
        tools = directory / "tools"
        tools.mkdir()
        (tools / "dbus-send").symlink_to(ENDPOINT)
        environment = os.environ | {
            "PATH": str(tools) + os.pathsep + os.environ["PATH"],
            "FPLINUX_TEST_POLICY_READY": str(directory / "policy-ready"),
            "FPLINUX_TEST_RELOAD_FAILURE": "1" if reject_reload else "0",
        }
        return subprocess.run(
            ["sh", "-c", '. "$1"; start_pre', "bluealsa-hook", str(SERVICE)],
            env=environment,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )

    def test_pre_start_reload_makes_the_installed_policy_available(self) -> None:
        """The hook must request the canonical system-bus reload before starting."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)

            result = self.run_hook(temporary, reject_reload=False)

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((temporary / "policy-ready").read_text(), "ready\n")
            self.assertEqual(result.stdout, "")

    def test_reload_rejection_is_returned_to_the_caller(self) -> None:
        """A rejected reload must stay visible instead of being swallowed."""
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)

            result = self.run_hook(temporary, reject_reload=True)

            self.assertEqual(result.returncode, 42)
            self.assertIn("policy reload rejected", result.stderr)
            self.assertFalse((temporary / "policy-ready").exists())

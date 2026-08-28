# SPDX-License-Identifier: GPL-2.0-only
"""Host-tool test for the generated OpenSSH configuration."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import ssh_transport

from tests.process import run_process
from tests.ssh_transport_support import create_ready_session

_SSH_CONFIG_TIMEOUT_SECONDS = 10


class SshTransportOpenSshConfigTests(unittest.TestCase):
    """Validate generated session configuration with the installed OpenSSH parser."""

    def setUp(self) -> None:
        """Create one complete ready session in an isolated runtime directory."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "runtime"
        self.root.mkdir(mode=0o700)

    def test_openssh_interprets_generated_private_session_config(self) -> None:
        """OpenSSH receives the bound endpoint and disables ambient forwarding state."""
        session = create_ready_session(self.root)
        with mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root):
            ssh_transport._mark_current(session)  # noqa: SLF001

            path = self.root / "current" / "phone.ssh-config"
            metadata = path.lstat()
            self.assertTrue(path.is_file())
            self.assertFalse(path.is_symlink())
            self.assertEqual(metadata.st_mode & 0o777, 0o600)

            ssh = shutil.which("ssh")
            self.assertIsNotNone(ssh)
            parsed = run_process(
                [str(ssh), "-G", "-F", str(path), "fplinux"],
                name="OpenSSH generated configuration parse",
                timeout=_SSH_CONFIG_TIMEOUT_SECONDS,
            )
            self.assertEqual(parsed.returncode, 0, parsed.stderr)
            effective = dict(line.split(maxsplit=1) for line in parsed.stdout.splitlines())
            self.assertEqual(effective["hostname"], session["phone_address"])
            self.assertEqual(effective["user"], "root")
            self.assertEqual(effective["identityfile"], session["private_key"])
            self.assertEqual(effective["userknownhostsfile"], session["known_hosts"])
            self.assertEqual(effective["hostkeyalias"], f"fplinux-{session['usb_serial']}")
            self.assertEqual(effective["bindaddress"], session["host_address"])
            self.assertEqual(effective["passwordauthentication"], "no")
            self.assertEqual(effective["identityagent"], "none")
            self.assertEqual(effective["forwardagent"], "no")
            self.assertNotIn("proxycommand", effective)

            ssh_transport._cleanup_target_sessions(self.root, "phone")  # noqa: SLF001

        self.assertFalse(path.exists())
        self.assertFalse((self.root / "current" / "phone.json").exists())


if __name__ == "__main__":
    unittest.main()

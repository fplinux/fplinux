# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for brightness lease, state and restart behavior."""

from __future__ import annotations

import signal
import subprocess
import time
import unittest

from tests.host_process.test_fplinux_brightness_cli import CONFIG, BrightnessProcesses


class FPLinuxBrightnessServiceTests(BrightnessProcesses):
    """Check the daemon's state and preview lifecycle on the host."""

    def run_once(self) -> subprocess.CompletedProcess[str]:
        """Run a second daemon attempt to completion."""
        return subprocess.run(
            [
                str(self.daemon),
                "--config",
                str(self.config),
                "--backlight",
                str(self.backlight),
                "--socket",
                str(self.socket),
                "--state",
                str(self.state),
            ],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )

    def test_preview_keeps_new_baseline_until_release(self) -> None:
        """SET during SHOW changes desired state and applies on RELEASE."""
        self.start_daemon()
        lease = self.client()
        other = self.client()
        self.assertEqual(self.exchange(lease, "CLAIM"), "OK")
        self.assertEqual(self.exchange(other, "CLAIM"), "ERR 16")
        self.assertEqual(self.exchange(other, "SHOW 2"), "ERR 1")
        self.assertEqual(self.exchange(lease, "SHOW 2"), "OK")
        self.assertEqual(self.raw(), "2\n")
        self.assertEqual(self.run_cli("set", "9").returncode, 0)
        self.assertEqual(self.exchange(other, "GET"), "LEVEL 9")
        self.assertEqual(self.raw(), "2\n")
        self.assertEqual(self.exchange(lease, "RELEASE"), "OK")
        self.assertEqual(self.raw(), "44\n")

    def test_preview_disconnect_restores_latest_baseline(self) -> None:
        """Closing a lease holder applies the latest desired level."""
        self.start_daemon()
        lease = self.client()
        self.assertEqual(self.exchange(lease, "CLAIM"), "OK")
        self.assertEqual(self.exchange(lease, "SHOW 0"), "OK")
        self.assertEqual(self.run_cli("set", "8").returncode, 0)
        lease.close()
        other = self.client()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and self.raw() != "32\n":
            time.sleep(0.01)
        self.assertEqual(self.raw(), "32\n")
        self.assertEqual(self.exchange(other, "CLAIM"), "OK")

    def test_killed_daemon_recovers_published_baseline_and_stale_socket(self) -> None:
        """Restart after a daemon killed during preview recovers the SET level."""
        self.start_daemon()
        self.assertEqual(self.run_cli("set", "10").returncode, 0)
        lease = self.client()
        self.assertEqual(self.exchange(lease, "CLAIM"), "OK")
        self.assertEqual(self.exchange(lease, "SHOW 0"), "OK")
        self.assertEqual(self.raw(), "0\n")
        process = self.process
        if process is None:
            self.fail("brightness daemon did not start")
        process.send_signal(signal.SIGKILL)
        process.communicate(timeout=3)
        self.assertTrue(self.socket.exists())
        self.start_daemon()
        self.assertEqual(self.run_cli("get").stdout, "10\n")
        self.assertEqual(self.raw(), "63\n")
        self.assertEqual(self.socket.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o600)

    def test_restart_keeps_the_selected_level(self) -> None:
        """A level set before SIGTERM is reported and applied after the service restarts."""
        self.start_daemon()
        self.assertEqual(self.run_cli("set", "4").returncode, 0)
        self.stop_daemon()
        self.start_daemon()
        self.assertEqual(self.run_cli("get").stdout, "4\n")
        self.assertEqual(self.raw(), "7\n")

    def test_invalid_config_and_unavailable_device_fail_without_writing(self) -> None:
        """Malformed configuration and missing sysfs values block startup."""
        invalid = (
            "backlight=../lcd\nlevels=0,1,2,4,7,11,16,23,32,44,63\n",
            "backlight=lcd\nlevels=0,1,2,4,7,11,16,23,32,44\n",
            "backlight=lcd\nlevels=0,1,2,4,7,11,16,23,32,44,64\n",
            "backlight=lcd\nlevels=0,1,2,4,7,11,11,23,32,44,63\n",
            "backlight=lcd\nlevels=1,2,3,4,7,11,16,23,32,44,63\n",
            CONFIG + "extra=yes\n",
            CONFIG.rstrip("\n"),
        )
        for config in invalid:
            with self.subTest(config=config):
                self.config.write_text(config, encoding="ascii")
                self.assertNotEqual(self.run_once().returncode, 0)
                self.assertEqual(self.raw(), "0\n")
        self.config.unlink()
        self.assertNotEqual(self.run_once().returncode, 0)
        self.config.write_text(CONFIG, encoding="ascii")
        (self.backlight / "max_brightness").write_text("62\n", encoding="ascii")
        self.assertNotEqual(self.run_once().returncode, 0)
        self.assertEqual(self.raw(), "0\n")
        (self.backlight / "max_brightness").unlink()
        self.assertNotEqual(self.run_once().returncode, 0)

    def test_singleton_rejects_second_daemon_without_changing_brightness(self) -> None:
        """A rejected second daemon keeps the preview and the level kept across restarts."""
        self.start_daemon()
        self.assertEqual(self.run_cli("set", "3").returncode, 0)
        lease = self.client()
        self.assertEqual(self.exchange(lease, "CLAIM"), "OK")
        self.assertEqual(self.exchange(lease, "SHOW 0"), "OK")
        self.assertEqual(self.raw(), "0\n")
        self.assertNotEqual(self.run_once().returncode, 0)
        self.assertEqual(self.raw(), "0\n")
        self.assertEqual(self.run_cli("get").stdout, "3\n")
        self.stop_daemon()
        self.start_daemon()
        self.assertEqual(self.run_cli("get").stdout, "3\n")


if __name__ == "__main__":
    unittest.main()

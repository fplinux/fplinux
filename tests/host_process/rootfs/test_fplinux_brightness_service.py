# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for brightness lease, state and restart behavior."""

from __future__ import annotations

import os
import signal
import subprocess
import time
from pathlib import Path
from typing import ClassVar

import pytest

from tests import ROOT
from tests.host_process.rootfs.test_fplinux_brightness_cli import (
    APORT,
    CONFIG,
    INCLUDE,
    LIB,
    BrightnessProcesses,
)
from tests.process import run_process


class FPLinuxBrightnessServiceTests(BrightnessProcesses):
    """Check the daemon's state and preview lifecycle on the host."""

    signal_daemon: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_signal_daemon(cls, _compiled_brightness: None) -> None:
        """Link a daemon whose wait boundary can receive a real stop signal."""
        cls.signal_daemon = Path(cls.build_dir.name) / "fplinux-brightnessd-stop-wait"
        run_process(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(INCLUDE),
                str(APORT / "fplinux-brightnessd.c"),
                str(LIB / "fplinux-cli.c"),
                str(ROOT / "tests/fixtures/processes/brightness_stop_wait.c"),
                "-Wl,--wrap=poll",
                "-Wl,--wrap=ppoll",
                "-Wl,--wrap=send",
                "-o",
                str(cls.signal_daemon),
            ],
            name="compile brightness daemon stop boundary",
            timeout=30,
            check=True,
        )

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
        assert (self.exchange(lease, "CLAIM")) == ("OK")
        assert (self.exchange(other, "CLAIM")) == ("ERR 16")
        assert (self.exchange(other, "SHOW 2")) == ("ERR 1")
        assert (self.exchange(lease, "SHOW 2")) == ("OK")
        assert (self.raw()) == ("2\n")
        assert (self.run_cli("set", "9").returncode) == (0)
        assert (self.exchange(other, "GET")) == ("LEVEL 9")
        assert (self.raw()) == ("2\n")
        assert (self.exchange(lease, "RELEASE")) == ("OK")
        assert (self.raw()) == ("44\n")

    def test_preview_disconnect_restores_latest_baseline(self) -> None:
        """Closing a lease holder applies the latest desired level."""
        self.start_daemon()
        lease = self.client()
        assert (self.exchange(lease, "CLAIM")) == ("OK")
        assert (self.exchange(lease, "SHOW 0")) == ("OK")
        assert (self.run_cli("set", "8").returncode) == (0)
        lease.close()
        other = self.client()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and self.raw() != "32\n":
            time.sleep(0.01)
        assert (self.raw()) == ("32\n")
        assert (self.exchange(other, "CLAIM")) == ("OK")

    def test_killed_daemon_recovers_published_baseline_and_stale_socket(self) -> None:
        """Restart after a daemon killed during preview recovers the SET level."""
        self.start_daemon()
        assert (self.run_cli("set", "10").returncode) == (0)
        lease = self.client()
        assert (self.exchange(lease, "CLAIM")) == ("OK")
        assert (self.exchange(lease, "SHOW 0")) == ("OK")
        assert (self.raw()) == ("0\n")
        process = self.process
        if process is None:
            pytest.fail("brightness daemon did not start")
        process.send_signal(signal.SIGKILL)
        process.communicate(timeout=3)
        assert self.socket.exists()
        self.start_daemon()
        assert (self.run_cli("get").stdout) == ("10\n")
        assert (self.raw()) == ("63\n")
        assert (self.socket.stat().st_mode & 0o777) == (0o600)
        assert (self.state.stat().st_mode & 0o777) == (0o600)

    def test_restart_keeps_the_selected_level(self) -> None:
        """A level set before SIGTERM is reported and applied after the service restarts."""
        self.start_daemon()
        assert (self.run_cli("set", "4").returncode) == (0)
        self.stop_daemon()
        self.start_daemon()
        assert (self.run_cli("get").stdout) == ("4\n")
        assert (self.raw()) == ("7\n")

    @pytest.mark.parametrize(
        ("signum", "preview"),
        [
            (signal.SIGTERM, False),
            (signal.SIGTERM, True),
            (signal.SIGINT, False),
            (signal.SIGINT, True),
        ],
        ids=["term-idle", "term-preview", "int-idle", "int-preview"],
    )
    def test_stop_signal_before_wait_exits_and_restores_selected_level(
        self, *, signum: signal.Signals, preview: bool
    ) -> None:
        """Real SIGTERM/SIGINT at an idle wait exits and cleans up an active lease."""
        request = self.work / "stop.request"
        notice = self.work / "stop.notice"
        environment = {
            **os.environ,
            "FPLINUX_TEST_STOP_REQUEST": str(request),
            "FPLINUX_TEST_STOP_NOTICE": str(notice),
        }
        self.stop_daemon()
        request.unlink(missing_ok=True)
        notice.unlink(missing_ok=True)
        self.start_daemon(binary=self.signal_daemon, env=environment)
        assert (self.run_cli("set", "4").returncode) == (0)
        client = self.client()
        if preview:
            assert (self.exchange(client, "CLAIM")) == ("OK")
            assert (self.exchange(client, "SHOW 0")) == ("OK")
            assert (self.raw()) == ("0\n")
        request.write_text(f"{signum}\n", encoding="ascii")
        assert (self.exchange(client, "GET")) == ("LEVEL 4")
        result = self.wait_for_daemon_exit()
        assert (result.returncode) == (0), result.stderr
        assert (notice.read_text(encoding="ascii")) == (f"{signum}\n")
        assert not (self.socket.exists())
        assert (self.state.read_text(encoding="ascii")) == ("4\n")
        assert (self.raw()) == ("7\n")
        client.close()
        self.start_daemon()
        assert (self.run_cli("get").stdout) == ("4\n")
        assert (self.raw()) == ("7\n")
        self.stop_daemon()

    @pytest.mark.parametrize(
        ("config", "maximum"),
        [
            ("backlight=../lcd\nlevels=0,1,2,4,7,11,16,23,32,44,63\n", "63\n"),
            ("backlight=lcd\nlevels=0,1,2,4,7,11,16,23,32,44\n", "63\n"),
            ("backlight=lcd\nlevels=0,1,2,4,7,11,16,23,32,44,64\n", "63\n"),
            ("backlight=lcd\nlevels=0,1,2,4,7,11,11,23,32,44,63\n", "63\n"),
            ("backlight=lcd\nlevels=1,2,3,4,7,11,16,23,32,44,63\n", "63\n"),
            (CONFIG + "extra=yes\n", "63\n"),
            (CONFIG.rstrip("\n"), "63\n"),
            (None, "63\n"),
            (CONFIG, "62\n"),
            (CONFIG, None),
        ],
        ids=[
            "invalid-backlight",
            "missing-level",
            "out-of-range-raw",
            "repeated-level",
            "nonzero-start",
            "extra-field",
            "missing-newline",
            "missing-config",
            "wrong-maximum",
            "missing-maximum",
        ],
    )
    def test_invalid_config_and_unavailable_device_fail_without_writing(
        self, config: str | None, maximum: str | None
    ) -> None:
        """Malformed configuration and missing sysfs values block startup."""
        if config is None:
            self.config.unlink()
        else:
            self.config.write_text(config, encoding="ascii")
        maximum_path = self.backlight / "max_brightness"
        if maximum is None:
            maximum_path.unlink()
        else:
            maximum_path.write_text(maximum, encoding="ascii")
        assert self.run_once().returncode != 0
        assert self.raw() == "0\n"

    def test_singleton_rejects_second_daemon_without_changing_brightness(self) -> None:
        """A rejected second daemon keeps the preview and the level kept across restarts."""
        self.start_daemon()
        assert (self.run_cli("set", "3").returncode) == (0)
        lease = self.client()
        assert (self.exchange(lease, "CLAIM")) == ("OK")
        assert (self.exchange(lease, "SHOW 0")) == ("OK")
        assert (self.raw()) == ("0\n")
        assert (self.run_once().returncode) != (0)
        assert (self.raw()) == ("0\n")
        assert (self.run_cli("get").stdout) == ("3\n")
        self.stop_daemon()
        self.start_daemon()
        assert (self.run_cli("get").stdout) == ("3\n")

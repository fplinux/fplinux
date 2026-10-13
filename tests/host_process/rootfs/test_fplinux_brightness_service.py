# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for brightness lease, state and restart behavior."""

from __future__ import annotations

import os
import signal
import time
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.rootfs.brightness import (
    APORT,
    CONFIG,
    INCLUDE,
    LIB,
)
from tests.fixtures.rootfs.brightness import (
    brightness as brightness,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.fixtures.rootfs.brightness import (
    brightness_binaries as brightness_binaries,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

    from tests.fixtures.rootfs.brightness import BrightnessBinaries, BrightnessProcesses


@pytest.fixture(scope="class")
def signal_daemon(brightness_binaries: BrightnessBinaries) -> Path:
    """Link a daemon whose wait boundary can receive a real stop signal."""
    executable = brightness_binaries.directory / "fplinux-brightnessd-stop-wait"
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
            str(executable),
        ],
        name="compile brightness daemon stop boundary",
        timeout=30,
        check=True,
    )
    return executable


class FPLinuxBrightnessServiceTests:
    """Check the daemon's state and preview lifecycle on the host."""

    def test_preview_keeps_new_baseline_until_release(
        self, brightness: BrightnessProcesses
    ) -> None:
        """SET during SHOW changes desired state and applies on RELEASE."""
        brightness.start_daemon()
        lease = brightness.client()
        other = brightness.client()
        assert (brightness.exchange(lease, "CLAIM")) == ("OK")
        assert (brightness.exchange(other, "CLAIM")) == ("ERR 16")
        assert (brightness.exchange(other, "SHOW 2")) == ("ERR 1")
        assert (brightness.exchange(lease, "SHOW 2")) == ("OK")
        assert (brightness.raw()) == ("2\n")
        assert (brightness.run_cli("set", "9").returncode) == (0)
        assert (brightness.exchange(other, "GET")) == ("LEVEL 9")
        assert (brightness.raw()) == ("2\n")
        assert (brightness.exchange(lease, "RELEASE")) == ("OK")
        assert (brightness.raw()) == ("44\n")

    def test_preview_disconnect_restores_latest_baseline(
        self, brightness: BrightnessProcesses
    ) -> None:
        """Closing a lease holder applies the latest desired level."""
        brightness.start_daemon()
        lease = brightness.client()
        assert (brightness.exchange(lease, "CLAIM")) == ("OK")
        assert (brightness.exchange(lease, "SHOW 0")) == ("OK")
        assert (brightness.run_cli("set", "8").returncode) == (0)
        lease.close()
        other = brightness.client()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and brightness.raw() != "32\n":
            time.sleep(0.01)
        assert (brightness.raw()) == ("32\n")
        assert (brightness.exchange(other, "CLAIM")) == ("OK")

    def test_killed_daemon_recovers_published_baseline_and_stale_socket(
        self, brightness: BrightnessProcesses
    ) -> None:
        """Restart after a daemon killed during preview recovers the SET level."""
        brightness.start_daemon()
        assert (brightness.run_cli("set", "10").returncode) == (0)
        lease = brightness.client()
        assert (brightness.exchange(lease, "CLAIM")) == ("OK")
        assert (brightness.exchange(lease, "SHOW 0")) == ("OK")
        assert (brightness.raw()) == ("0\n")
        process = brightness.process
        if process is None:
            pytest.fail("brightness daemon did not start")
        process.send_signal(signal.SIGKILL)
        process.communicate(timeout=3)
        assert brightness.socket_path.exists()
        brightness.start_daemon()
        assert (brightness.run_cli("get").stdout) == ("10\n")
        assert (brightness.raw()) == ("63\n")
        assert (brightness.socket_path.stat().st_mode & 0o777) == (0o600)
        assert (brightness.state.stat().st_mode & 0o777) == (0o600)

    def test_restart_keeps_the_selected_level(self, brightness: BrightnessProcesses) -> None:
        """A level set before SIGTERM is reported and applied after the service restarts."""
        brightness.start_daemon()
        assert (brightness.run_cli("set", "4").returncode) == (0)
        brightness.stop_daemon()
        brightness.start_daemon()
        assert (brightness.run_cli("get").stdout) == ("4\n")
        assert (brightness.raw()) == ("7\n")

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
        self,
        brightness: BrightnessProcesses,
        signal_daemon: Path,
        *,
        signum: signal.Signals,
        preview: bool,
    ) -> None:
        """Real SIGTERM/SIGINT at an idle wait exits and cleans up an active lease."""
        request = brightness.work / "stop.request"
        notice = brightness.work / "stop.notice"
        environment = {
            **os.environ,
            "FPLINUX_TEST_STOP_REQUEST": str(request),
            "FPLINUX_TEST_STOP_NOTICE": str(notice),
        }
        brightness.stop_daemon()
        request.unlink(missing_ok=True)
        notice.unlink(missing_ok=True)
        brightness.start_daemon(binary=signal_daemon, env=environment)
        assert (brightness.run_cli("set", "4").returncode) == (0)
        client = brightness.client()
        if preview:
            assert (brightness.exchange(client, "CLAIM")) == ("OK")
            assert (brightness.exchange(client, "SHOW 0")) == ("OK")
            assert (brightness.raw()) == ("0\n")
        request.write_text(f"{signum}\n", encoding="ascii")
        assert (brightness.exchange(client, "GET")) == ("LEVEL 4")
        result = brightness.wait_for_daemon_exit()
        assert (result.returncode) == (0), result.stderr
        assert (notice.read_text(encoding="ascii")) == (f"{signum}\n")
        assert not (brightness.socket_path.exists())
        assert (brightness.state.read_text(encoding="ascii")) == ("4\n")
        assert (brightness.raw()) == ("7\n")
        client.close()
        brightness.start_daemon()
        assert (brightness.run_cli("get").stdout) == ("4\n")
        assert (brightness.raw()) == ("7\n")
        brightness.stop_daemon()

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
        self, brightness: BrightnessProcesses, config: str | None, maximum: str | None
    ) -> None:
        """Malformed configuration and missing sysfs values block startup."""
        if config is None:
            brightness.config.unlink()
        else:
            brightness.config.write_text(config, encoding="ascii")
        maximum_path = brightness.backlight / "max_brightness"
        if maximum is None:
            maximum_path.unlink()
        else:
            maximum_path.write_text(maximum, encoding="ascii")
        assert brightness.run_once().returncode != 0
        assert brightness.raw() == "0\n"

    def test_singleton_rejects_second_daemon_without_changing_brightness(
        self, brightness: BrightnessProcesses
    ) -> None:
        """A rejected second daemon keeps the preview and the level kept across restarts."""
        brightness.start_daemon()
        assert (brightness.run_cli("set", "3").returncode) == (0)
        lease = brightness.client()
        assert (brightness.exchange(lease, "CLAIM")) == ("OK")
        assert (brightness.exchange(lease, "SHOW 0")) == ("OK")
        assert (brightness.raw()) == ("0\n")
        assert (brightness.run_once().returncode) != (0)
        assert (brightness.raw()) == ("0\n")
        assert (brightness.run_cli("get").stdout) == ("3\n")
        brightness.stop_daemon()
        brightness.start_daemon()
        assert (brightness.run_cli("get").stdout) == ("3\n")

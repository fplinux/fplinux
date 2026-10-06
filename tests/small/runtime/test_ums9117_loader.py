# SPDX-License-Identifier: GPL-2.0-only
"""UMS9117 adapter loader behavior."""

from __future__ import annotations

import contextlib
import io
import signal
import subprocess
import tempfile
from pathlib import Path
from unittest import mock

import pytest

from tests.small.runtime.adapter_fixtures import ADAPTER, BridgeFixture, BridgeProcess, FakeClock


class LoaderArgumentsTests:
    """Keep exec-distance selection entirely target-driven."""

    def test_nonzero_exec_distance_precedes_ram_loads(self) -> None:
        """Emit the optional setup command before both RAM FDL commands."""
        arguments = ADAPTER.loader_arguments(
            Path("loader"),
            Path("fdl1.bin"),
            Path("image.bin"),
            {"fdl1": 0x6200, "payload": 0x80100000},
            0x314D,
        )
        assert (arguments) == (
            [
                "loader",
                "--wait",
                "0",
                "t117_exec_dist",
                "0x314d",
                "fdl",
                "fdl1.bin",
                "0x6200",
                "fdl",
                "image.bin",
                "0x80100000",
            ]
        )

    def test_zero_exec_distance_omits_setup_command(self) -> None:
        """Start directly with the RAM FDL load when the target requests zero."""
        arguments = ADAPTER.loader_arguments(
            Path("loader"),
            Path("fdl1.bin"),
            Path("image.bin"),
            {"fdl1": 0x6200, "payload": 0x80100000},
            0,
        )
        assert (arguments) == (
            [
                "loader",
                "--wait",
                "0",
                "fdl",
                "fdl1.bin",
                "0x6200",
                "fdl",
                "image.bin",
                "0x80100000",
            ]
        )


class LoaderExecutionTests(BridgeFixture):
    """The loader result, bridge exit status and BootROM release gate the Linux transition."""

    def test_transient_bootrom_node_reaches_loader_without_settle_delay(self) -> None:
        """Open a detected short-lived BootROM node before it disconnects."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            bootrom = bundle / "bootrom-usbfs"
            bootrom.write_bytes(b"")
            transport_module = mock.Mock()

            def expire_on_long_sleep(seconds: float) -> None:
                if seconds >= 0.5:
                    bootrom.unlink(missing_ok=True)

            def run_loader(
                *_args: object, **_kwargs: object
            ) -> subprocess.CompletedProcess[bytes]:
                bootrom.unlink()
                return subprocess.CompletedProcess([], 0)

            with (
                contextlib.redirect_stdout(io.StringIO()),
                mock.patch.object(ADAPTER, "usb_device_path", return_value=bootrom),
                mock.patch.object(ADAPTER.time, "sleep", side_effect=expire_on_long_sleep),
                mock.patch.object(ADAPTER.subprocess, "run", side_effect=run_loader),
                mock.patch.object(
                    ADAPTER.subprocess,
                    "Popen",
                    return_value=BridgeProcess(0),
                ),
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
            ):
                ADAPTER.run(
                    bundle, self.runtime("none"), self.session(), expected_device_identity="9" * 64
                )

        transport_module.remove_personalized_image.assert_called_once()

    def test_loader_retries_after_a_fresh_bootrom_reconnect(self) -> None:
        """A transient RAM transfer failure waits for one fresh USB device."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            devices = [bundle / "bootrom-1", bundle / "bootrom-2"]
            devices[0].write_bytes(b"")
            transport_module = mock.Mock()
            loader_runs = 0

            def run_loader(
                *_args: object, **_kwargs: object
            ) -> subprocess.CompletedProcess[bytes]:
                nonlocal loader_runs
                devices[loader_runs].unlink()
                loader_runs += 1
                if loader_runs == 1:
                    devices[1].write_bytes(b"")
                return subprocess.CompletedProcess([], 1 if loader_runs == 1 else 0)

            with (
                contextlib.redirect_stdout(io.StringIO()),
                mock.patch.object(ADAPTER, "usb_device_path", side_effect=devices),
                mock.patch.object(ADAPTER.subprocess, "run", side_effect=run_loader),
                mock.patch.object(
                    ADAPTER.subprocess,
                    "Popen",
                    return_value=BridgeProcess(0),
                ),
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
            ):
                ADAPTER.run(
                    bundle, self.runtime("none"), self.session(), expected_device_identity="9" * 64
                )

        assert (loader_runs) == (2)
        transport_module.remove_personalized_image.assert_called_once()

    def test_loader_stops_after_three_failed_fresh_devices(self) -> None:
        """Bound transient RAM loader retries to three physical attempts."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            devices = [bundle / f"bootrom-{index}" for index in range(3)]
            devices[0].write_bytes(b"")
            transport_module = mock.Mock()
            loader_runs = 0

            def fail_loader(
                *_args: object, **_kwargs: object
            ) -> subprocess.CompletedProcess[bytes]:
                nonlocal loader_runs
                devices[loader_runs].unlink()
                loader_runs += 1
                if loader_runs < len(devices):
                    devices[loader_runs].write_bytes(b"")
                return subprocess.CompletedProcess([], 1)

            with (
                contextlib.redirect_stdout(io.StringIO()),
                mock.patch.object(ADAPTER, "usb_device_path", side_effect=devices),
                mock.patch.object(ADAPTER.subprocess, "run", side_effect=fail_loader),
                mock.patch.object(ADAPTER.subprocess, "Popen") as popen,
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
                pytest.raises(SystemExit, match="failed after 3 attempts"),
            ):
                ADAPTER.run(
                    bundle, self.runtime("none"), self.session(), expected_device_identity="9" * 64
                )

        assert (loader_runs) == (3)
        popen.assert_not_called()
        transport_module.remove_personalized_image.assert_called_once()

    def test_discovery_race_does_not_consume_a_loader_attempt(self) -> None:
        """Wait for a fresh node when the first vanishes before usbfs access."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            devices = [bundle / "bootrom-1", bundle / "bootrom-2"]
            devices[0].write_bytes(b"")
            transport_module = mock.Mock()
            access_calls = 0
            loader_runs = 0

            def access(device: Path, _identity: str) -> None:
                nonlocal access_calls
                access_calls += 1
                if device == devices[0]:
                    devices[0].unlink()
                    devices[1].write_bytes(b"")
                    raise ADAPTER.BootromDisconnectedError

            def run_loader(
                *_args: object, **_kwargs: object
            ) -> subprocess.CompletedProcess[bytes]:
                nonlocal loader_runs
                devices[1].unlink()
                loader_runs += 1
                return subprocess.CompletedProcess([], 0)

            with (
                contextlib.redirect_stdout(io.StringIO()),
                mock.patch.object(ADAPTER, "usb_device_path", side_effect=devices),
                mock.patch.object(
                    ADAPTER,
                    "require_usb_device_access",
                    side_effect=access,
                ),
                mock.patch.object(ADAPTER.subprocess, "run", side_effect=run_loader),
                mock.patch.object(
                    ADAPTER.subprocess,
                    "Popen",
                    return_value=BridgeProcess(0),
                ),
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
            ):
                ADAPTER.run(
                    bundle, self.runtime("none"), self.session(), expected_device_identity="9" * 64
                )

        assert (access_calls) == (2)
        assert (loader_runs) == (1)
        transport_module.remove_personalized_image.assert_called_once()

    def test_retry_waits_for_the_old_usb_node_to_disappear(self) -> None:
        """Do not launch attempt two against the failed attempt's USB node."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            bootrom = bundle / "bootrom"
            bootrom.write_bytes(b"")
            transport_module = mock.Mock()
            loader = mock.Mock(return_value=subprocess.CompletedProcess([], 1))
            clock = FakeClock()

            with (
                contextlib.redirect_stdout(io.StringIO()),
                mock.patch.object(ADAPTER, "usb_device_path", return_value=bootrom),
                mock.patch.object(ADAPTER.subprocess, "run", loader),
                mock.patch.object(ADAPTER.subprocess, "Popen") as popen,
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
                mock.patch.object(ADAPTER.time, "monotonic", clock.monotonic),
                mock.patch.object(ADAPTER.time, "sleep", clock.sleep),
                pytest.raises(SystemExit, match="retry deadline"),
            ):
                ADAPTER.run(
                    bundle, self.runtime("none"), self.session(), expected_device_identity="9" * 64
                )

        loader.assert_called_once()
        popen.assert_not_called()
        transport_module.remove_personalized_image.assert_called_once()

    def test_loader_cancellation_is_not_retried(self) -> None:
        """Propagate cancellation and clean the prepared session exactly once."""
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            bootrom = bundle / "bootrom"
            bootrom.write_bytes(b"")
            transport_module = mock.Mock()
            loader = mock.Mock(side_effect=SystemExit("cancelled"))

            with (
                contextlib.redirect_stdout(io.StringIO()),
                mock.patch.object(ADAPTER, "usb_device_path", return_value=bootrom),
                mock.patch.object(ADAPTER.subprocess, "run", loader),
                mock.patch.object(ADAPTER.subprocess, "Popen") as popen,
                mock.patch.object(ADAPTER.shutil, "which", return_value="/usr/bin/stdbuf"),
                mock.patch.object(
                    ADAPTER.importlib,
                    "import_module",
                    return_value=transport_module,
                ),
                pytest.raises(SystemExit, match="cancelled"),
            ):
                ADAPTER.run(
                    bundle, self.runtime("none"), self.session(), expected_device_identity="9" * 64
                )

        loader.assert_called_once()
        popen.assert_not_called()
        transport_module.remove_personalized_image.assert_called_once()

    def test_adapter_preserves_runner_owned_signal_handlers(self) -> None:
        """The adapter must not replace the outer runner's process-lifecycle owner."""
        signals = (signal.SIGINT, signal.SIGTERM)
        before = {signum: signal.getsignal(signum) for signum in signals}
        try:
            self.run_bridge(BridgeProcess(0), bootrom_release_seconds=1.0)
            assert ({signum: signal.getsignal(signum) for signum in signals}) == (before)
        finally:
            for signum, handler in before.items():
                signal.signal(signum, handler)

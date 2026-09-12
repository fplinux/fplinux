# SPDX-License-Identifier: GPL-2.0-only
"""Private-bus and synthetic-sysfs checks for the FPLinux Bluetooth client."""

from __future__ import annotations

import os
import select
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from contextlib import suppress
from pathlib import Path
from typing import ClassVar

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
CLIENT_SOURCE = ROOT / "alpine/aports/fplinux-bluetooth/fplinux-bluetooth.c"
SERVICE_SOURCE = ROOT / "tests/host_tool/fplinux-bluetooth-service.c"
PEER = "01:23:45:67:89:AB"
CONNECTED = "connected 01:23:45:67:89:AB via bnep0; configure IP, DHCP and NAT separately\n"


class FplinuxBluetoothHostToolTests(unittest.TestCase):
    """Exercise the client against a controlled D-Bus BlueZ/obexd fake."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    work: ClassVar[Path]
    client: ClassVar[Path]
    service: ClassVar[Path]
    driver_directory: ClassVar[Path]
    bus: subprocess.Popen[str]
    fake: subprocess.Popen[str]
    case: tempfile.TemporaryDirectory[str]
    address: str

    @classmethod
    def setUpClass(cls) -> None:
        """Build the real client and the test-owned fake through pkg-config."""
        if shutil.which("dbus-daemon") is None:
            message = "dbus-daemon is required for Bluetooth host tests"
            raise unittest.SkipTest(message)
        try:
            cflags = run_process(
                ["pkg-config", "--cflags", "dbus-1"],
                name="read D-Bus compiler flags",
                timeout=10,
                check=True,
            ).stdout.split()
            libraries = run_process(
                ["pkg-config", "--libs", "dbus-1"],
                name="read D-Bus linker flags",
                timeout=10,
                check=True,
            ).stdout.split()
        except (FileNotFoundError, subprocess.CalledProcessError) as error:
            message = "pkg-config dbus-1 development files are required"
            raise unittest.SkipTest(message) from error

        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.work = Path(cls.temporary.name)
        cls.client = cls.work / "fplinux-bluetooth"
        cls.service = cls.work / "fplinux-bluetooth-service"
        cls.driver_directory = cls.work / "platform-driver"
        for source, output, name in (
            (CLIENT_SOURCE, cls.client, "compile FPLinux Bluetooth client"),
            (SERVICE_SOURCE, cls.service, "compile FPLinux Bluetooth test service"),
        ):
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f'-DFPLINUX_BLUETOOTH_DRIVER_DIR="{cls.driver_directory}"',
                    *cflags,
                    str(source),
                    "-o",
                    str(output),
                    *libraries,
                ],
                name=name,
                timeout=30,
                check=True,
            )

    def setUp(self) -> None:
        """Create one isolated daemon and one fake service per scenario."""
        self.case = tempfile.TemporaryDirectory()
        self.addCleanup(self.case.cleanup)
        self.bus = subprocess.Popen(
            ["dbus-daemon", "--session", "--nofork", "--print-address=1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        self.addCleanup(self.stop_process, self.bus)
        if self.bus.stdout is None:
            self.fail("private D-Bus daemon stdout is not captured")
        self.address = self.bus.stdout.readline().strip()
        if not self.address:
            self.fail("private D-Bus daemon did not publish an address")

    @staticmethod
    def stop_process(process: subprocess.Popen[str]) -> None:
        """Stop an owned process group, matching the host-test process boundary."""
        if process.poll() is not None:
            process.communicate()
            return
        with suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGTERM)
        try:
            process.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate()

    def child_environment(self) -> dict[str, str]:
        """Pass the private address only to the client and its fake service."""
        environment = os.environ.copy()
        environment["DBUS_SYSTEM_BUS_ADDRESS"] = self.address
        environment["DBUS_SESSION_BUS_ADDRESS"] = self.address
        return environment

    def start_service(self, mode: str) -> None:
        """Start the external fake and wait for its explicit readiness marker."""
        ready = Path(self.case.name) / "service-ready"
        self.fake = subprocess.Popen(
            [str(self.service), mode, str(ready)],
            env=self.child_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        self.addCleanup(self.stop_process, self.fake)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if ready.exists():
                self.assertEqual(ready.read_text(encoding="ascii"), "ready\n")
                return
            if self.fake.poll() is not None:
                stdout, stderr = self.fake.communicate()
                self.fail(f"Bluetooth fake exited before readiness:\n{stdout}\n{stderr}")
            time.sleep(0.01)
        self.fail("Bluetooth fake did not publish readiness")

    def run_client(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Invoke the production client on this test's isolated system bus."""
        return run_process(
            [str(self.client), *arguments],
            name="run FPLinux Bluetooth client",
            timeout=5,
            env=self.child_environment(),
        )

    def prepare_driver_directory(self) -> Path:
        """Replace only device discovery with an owned temporary filesystem."""
        self.driver_directory.mkdir()
        self.addCleanup(shutil.rmtree, self.driver_directory)
        return self.driver_directory

    def test_enable_writes_one_start_without_a_bus_service(self) -> None:
        """A bound controller receives the start request without BlueZ running."""
        directory = self.prepare_driver_directory()
        device = directory / "400a0000.bluetooth"
        device.mkdir()
        start = device / "start"
        start.write_bytes(b"")
        (directory / "uevent").write_text("", encoding="ascii")

        result = self.run_client("enable")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(start.read_bytes(), b"1\n")
        self.assertIn("CM4 transport started", result.stdout)

    def test_enable_missing_board_is_optional_only_when_requested(self) -> None:
        """Boot may omit an unsupported controller; explicit enable must explain it."""
        result = self.run_client("enable")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not configured", result.stderr)

        optional = self.run_client("enable", "--if-present")
        self.assertEqual(optional.returncode, 0, optional.stderr)

    def test_enable_ambiguous_controller_does_not_start_either(self) -> None:
        """An ambiguous discovery result cannot initialize an arbitrary controller."""
        directory = self.prepare_driver_directory()
        controls = []
        for name in ("controller-a", "controller-b"):
            device = directory / name
            device.mkdir()
            start = device / "start"
            start.write_bytes(b"")
            controls.append(start)

        result = self.run_client("enable", "--if-present")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("multiple CM4", result.stderr)
        self.assertEqual([path.read_bytes() for path in controls], [b"", b""])

    def test_send_accepts_terminal_signal_before_sendfile_reply(self) -> None:
        """A fast completed transfer is retained until its object path arrives."""
        payload = Path(self.case.name) / "payload"
        payload.write_bytes(b"")
        self.start_service("send-fast")

        result = self.run_client("send", PEER, str(payload))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"sent {payload} to {PEER}\n")

    def test_receive_uses_name_before_authorization_and_writes_fixture_bytes(self) -> None:
        """The advertised Name determines the authorized receive target."""
        destination = Path(self.case.name) / "received"
        destination.mkdir()
        self.start_service("receive")

        result = self.run_client("receive", PEER, str(destination), "3")

        received = destination / "incoming.txt"
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(received.read_bytes(), b"fixture")
        self.assertIn(f"received {received}\n", result.stdout)

    def test_receive_collision_preserves_existing_destination_bytes(self) -> None:
        """A completed incoming transfer never replaces an existing file."""
        destination = Path(self.case.name) / "received"
        destination.mkdir()
        existing = destination / "incoming.txt"
        existing.write_bytes(b"old-fixture")
        self.start_service("receive")

        result = self.run_client("receive", PEER, str(destination), "3")

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(existing.read_bytes(), b"old-fixture")
        self.assertIn("refusing to overwrite incoming.txt", result.stderr)

    def test_network_rejects_disconnect_before_connect_reply(self) -> None:
        """A PAN link already down at Connect completion has no usable output."""
        self.start_service("network-early")

        result = self.run_client("network", PEER)

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("PAN disconnected before its interface could be used", result.stderr)

    def test_network_hides_interface_when_connected_snapshot_disappears(self) -> None:
        """A lost BlueZ device object cannot produce a usable PAN interface."""
        self.start_service("network-snapshot-missing")

        result = self.run_client("network", PEER)

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("cannot read PAN Connected state", result.stderr)

    def assert_network_connection_is_visible_before_link_loss(self, peer: str) -> None:
        """Observe one flushed PAN line while the client is still alive."""
        self.start_service("network-late")
        observed: list[str] = []

        def observe_connection(process: subprocess.Popen[str], deadline: float) -> None:
            if process.stdout is None:
                self.fail("Bluetooth PAN client stdout is not captured")
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    self.fail("Bluetooth PAN client exited before reporting its connection")
                remaining = max(0, deadline - time.monotonic())
                readable, _, _ = select.select([process.stdout], [], [], min(0.05, remaining))
                if readable:
                    observed.append(process.stdout.readline())
                    self.assertIsNone(process.poll())
                    os.kill(self.fake.pid, signal.SIGUSR1)
                    return
            self.fail("Bluetooth PAN client did not flush its connection line")

        result = run_process(
            [str(self.client), "network", peer],
            name="run FPLinux Bluetooth PAN client",
            timeout=5,
            env=self.child_environment(),
            while_running=observe_connection,
        )

        self.assertEqual(observed, [CONNECTED.replace(PEER, peer)])
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_network_flushes_connection_before_later_disconnect(self) -> None:
        """A non-TTY consumer sees the live PAN line before the link drops."""
        self.assert_network_connection_is_visible_before_link_loss(PEER)

    def test_network_normalizes_lowercase_peer_before_visible_link_state(self) -> None:
        """A lowercase peer reaches BlueZ's uppercase object path and stays observable."""
        self.assert_network_connection_is_visible_before_link_loss(PEER.lower())


if __name__ == "__main__":
    unittest.main()

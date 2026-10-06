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
from contextlib import ExitStack, suppress
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

CLIENT_SOURCE = ROOT / "alpine/aports/fplinux-bluetooth/fplinux-bluetooth.c"
SHARED_INCLUDE = ROOT / "include/fplinux"
SERVICE_SOURCE = ROOT / "tests/host_tool/media/fplinux-bluetooth-service.c"
PEER = "01:23:45:67:89:AB"
CONNECTED = "connected 01:23:45:67:89:AB via bnep0; configure IP, DHCP and NAT separately\n"
# No service directories: a request to an absent name fails instead of
# activating a BlueZ or obexd service installed on the host.
BUS_CONFIG = """\
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:dir={directory}</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"""


class FplinuxBluetoothHostToolTests:
    """Exercise the client against a controlled D-Bus BlueZ/obexd fake."""

    temporary: ClassVar[tempfile.TemporaryDirectory[str]]
    work: ClassVar[Path]
    client: ClassVar[Path]
    service: ClassVar[Path]
    driver_directory: Path
    bus: subprocess.Popen[str]
    fake: subprocess.Popen[str]
    case: tempfile.TemporaryDirectory[str]
    address: str
    cleanup: ExitStack

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_tools(cls) -> Iterator[None]:
        """Build the real client and the test-owned fake through pkg-config."""
        with ExitStack() as cleanup:
            if shutil.which("dbus-daemon") is None:
                message = "dbus-daemon is required for Bluetooth host tests"
                pytest.skip(message)
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
            except FileNotFoundError, subprocess.CalledProcessError:
                message = "pkg-config dbus-1 development files are required"
                pytest.skip(message)

            cls.temporary = tempfile.TemporaryDirectory()
            cleanup.enter_context(cls.temporary)
            cls.work = Path(cls.temporary.name)
            cls.client = cls.work / "fplinux-bluetooth"
            cls.service = cls.work / "fplinux-bluetooth-service"
            for source, output, name, extra_sources in (
                (
                    CLIENT_SOURCE,
                    cls.client,
                    "compile FPLinux Bluetooth client",
                    [
                        str(CLIENT_SOURCE.with_name("fplinux-bluetooth-opp.c")),
                        str(CLIENT_SOURCE.with_name("fplinux-bluetooth-pan.c")),
                        str(CLIENT_SOURCE.with_name("fplinux-bluetooth-common.c")),
                        str(ROOT / "lib/fplinux/fplinux-cli.c"),
                    ],
                ),
                (SERVICE_SOURCE, cls.service, "compile FPLinux Bluetooth test service", []),
            ):
                run_process(
                    [
                        "cc",
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        f"-I{SHARED_INCLUDE}",
                        '-DFPLINUX_BLUETOOTH_DRIVER_DIR="platform-driver"',
                        *cflags,
                        str(source),
                        *extra_sources,
                        "-o",
                        str(output),
                        *libraries,
                    ],
                    name=name,
                    timeout=30,
                    check=True,
                )
            yield

    @pytest.fixture(autouse=True)
    def _case_resources(self) -> Iterator[None]:
        """Create one isolated daemon and one fake service per scenario."""
        with ExitStack() as self.cleanup:
            self.case = tempfile.TemporaryDirectory()
            self.cleanup.enter_context(self.case)
            self.driver_directory = Path(self.case.name) / "platform-driver"
            configuration = Path(self.case.name) / "bus.conf"
            configuration.write_text(BUS_CONFIG.format(directory=self.case.name), encoding="utf-8")
            self.bus = subprocess.Popen(
                ["dbus-daemon", f"--config-file={configuration}", "--nofork", "--print-address=1"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            self.cleanup.callback(self.stop_process, self.bus)
            if self.bus.stdout is None:
                pytest.fail("private D-Bus daemon stdout is not captured")
            readable, _, _ = select.select([self.bus.stdout], [], [], 3)
            self.address = self.bus.stdout.readline().strip() if readable else ""
            if not self.address:
                with suppress(ProcessLookupError):
                    os.killpg(self.bus.pid, signal.SIGKILL)
                _, stderr = self.bus.communicate()
                pytest.fail(f"private D-Bus daemon did not publish an address:\n{stderr}")
            yield

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
        self.cleanup.callback(self.stop_process, self.fake)
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if ready.exists():
                assert (ready.read_text(encoding="ascii")) == ("ready\n")
                return
            if self.fake.poll() is not None:
                stdout, stderr = self.fake.communicate()
                pytest.fail(f"Bluetooth fake exited before readiness:\n{stdout}\n{stderr}")
            time.sleep(0.01)
        pytest.fail("Bluetooth fake did not publish readiness")

    def run_client(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Invoke the production client on this test's isolated system bus."""
        return run_process(
            [str(self.client), *arguments],
            name="run FPLinux Bluetooth client",
            timeout=5,
            env=self.child_environment(),
            cwd=Path(self.case.name),
        )

    def prepare_driver_directory(self) -> Path:
        """Replace only device discovery with an owned temporary filesystem."""
        self.driver_directory.mkdir()
        self.cleanup.callback(shutil.rmtree, self.driver_directory)
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

        assert (result.returncode) == (0), result.stderr
        assert (start.read_bytes()) == (b"1\n")
        assert (result.stdout) == (
            "Bluetooth interfaces ready; use bluetoothctl for adapter power\n"
        )

    @pytest.mark.parametrize(
        "arguments",
        [
            ("-h",),
            ("--help",),
            ("enable", "--help"),
            ("enable", "--if-present", "-h"),
            ("enable", "--unknown", "--help"),
            ("send", "--help"),
            ("receive", PEER, "--help"),
            ("network", "--help"),
        ],
        ids=[
            "0-h",
            "1-help",
            "2-enable",
            "3-enable---if-present",
            "4-enable---unknown",
            "5-send",
            "6-receive-PEER---help",
            "7-network",
        ],
    )
    def test_help_leaves_a_bound_controller_stopped(self, arguments: tuple[str, ...]) -> None:
        """Root and subcommand help cannot write the synthetic start control."""
        directory = self.prepare_driver_directory()
        device = directory / "400a0000.bluetooth"
        device.mkdir()
        start = device / "start"
        start.write_bytes(b"")

        result = self.run_client(*arguments)

        assert (result.returncode) == (0), result.stderr
        assert ("Usage:") in (result.stdout)
        assert ("--help") in (result.stdout)
        assert (result.stderr) == ("")
        assert (start.read_bytes()) == (b"")

    @pytest.mark.parametrize(
        "arguments",
        [
            (),
            ("unknown",),
            ("enable", "extra"),
            ("enable", "--if-present=value"),
            ("send", PEER),
            ("send", "invalid-peer", "unused"),
            ("network", PEER, "extra"),
            ("receive", PEER, "/unused", "0"),
            ("receive", PEER, "/unused", "3601"),
            ("receive", PEER, "/unused", "1tail"),
            ("receive", PEER, "/unused", "99999999999999999999999"),
            ("enable", "--", "--help"),
        ],
        ids=[
            "0-empty",
            "1-unknown",
            "2-enable",
            "3-enable",
            "4-send-PEER",
            "5-send-invalid-peer",
            "6-network-PEER-extra",
            "7-receive-PEER-unused-0",
            "8-receive-PEER-unused-3601",
            "9-receive-PEER-unused-1tail",
            "10-receive-PEER-unused-99999999999999999999999",
            "11-enable",
        ],
    )
    def test_invalid_command_arguments_are_diagnosed_without_bluez(
        self, arguments: tuple[str, ...]
    ) -> None:
        """Syntax and value errors are diagnosed with no running BlueZ service."""
        result = self.run_client(*arguments)

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--help") in (result.stderr)

    def test_enable_missing_board_is_optional_only_when_requested(self) -> None:
        """Boot may omit an unsupported controller; explicit enable must explain it."""
        result = self.run_client("enable")
        assert (result.returncode) != (0)
        assert ("not configured") in (result.stderr)

        optional = self.run_client("enable", "--if-present")
        assert (optional.returncode) == (0), optional.stderr

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

        assert (result.returncode) != (0)
        assert ("multiple CM4") in (result.stderr)
        assert ([path.read_bytes() for path in controls]) == ([b"", b""])

    def test_send_accepts_terminal_signal_before_sendfile_reply(self) -> None:
        """A fast completed transfer is retained until its object path arrives."""
        payload = Path(self.case.name) / "payload"
        payload.write_bytes(b"")
        self.start_service("send-fast")

        result = self.run_client("send", PEER, str(payload))

        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == (f"sent {payload} to {PEER}\n")

    def test_receive_uses_name_before_authorization_and_writes_fixture_bytes(self) -> None:
        """The advertised Name determines the authorized receive target."""
        destination = Path(self.case.name) / "received"
        destination.mkdir()
        self.start_service("receive")

        result = self.run_client("receive", PEER, str(destination), "3")

        received = destination / "incoming.txt"
        assert (result.returncode) == (0), result.stderr
        assert (received.read_bytes()) == (b"fixture")
        assert (f"received {received}\n") in (result.stdout)

    def test_receive_collision_preserves_existing_destination_bytes(self) -> None:
        """A completed incoming transfer never replaces an existing file."""
        destination = Path(self.case.name) / "received"
        destination.mkdir()
        existing = destination / "incoming.txt"
        existing.write_bytes(b"old-fixture")
        self.start_service("receive")

        result = self.run_client("receive", PEER, str(destination), "3")

        assert (result.returncode) != (0)
        assert (existing.read_bytes()) == (b"old-fixture")
        assert ("refusing to overwrite incoming.txt") in (result.stderr)

    def test_network_rejects_disconnect_before_connect_reply(self) -> None:
        """A PAN link already down at Connect completion has no usable output."""
        self.start_service("network-early")

        result = self.run_client("network", PEER)

        assert (result.returncode) != (0)
        assert (result.stdout) == ("")
        assert ("PAN disconnected before its interface could be used") in (result.stderr)

    def test_network_hides_interface_when_connected_snapshot_disappears(self) -> None:
        """A lost BlueZ device object cannot produce a usable PAN interface."""
        self.start_service("network-snapshot-missing")

        result = self.run_client("network", PEER)

        assert (result.returncode) != (0)
        assert (result.stdout) == ("")
        assert ("cannot read PAN Connected state") in (result.stderr)

    def assert_network_connection_is_visible_before_link_loss(self, peer: str) -> None:
        """Observe one flushed PAN line while the client is still alive."""
        self.start_service("network-late")
        observed: list[str] = []

        def observe_connection(process: subprocess.Popen[str], deadline: float) -> None:
            if process.stdout is None:
                pytest.fail("Bluetooth PAN client stdout is not captured")
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    pytest.fail("Bluetooth PAN client exited before reporting its connection")
                remaining = max(0, deadline - time.monotonic())
                readable, _, _ = select.select([process.stdout], [], [], min(0.05, remaining))
                if readable:
                    observed.append(process.stdout.readline())
                    assert (process.poll()) is None
                    os.kill(self.fake.pid, signal.SIGUSR1)
                    return
            pytest.fail("Bluetooth PAN client did not flush its connection line")

        result = run_process(
            [str(self.client), "network", peer],
            name="run FPLinux Bluetooth PAN client",
            timeout=5,
            env=self.child_environment(),
            while_running=observe_connection,
        )

        assert (observed) == ([CONNECTED.replace(PEER, peer)])
        assert (result.returncode) == (0), result.stderr

    def test_network_flushes_connection_before_later_disconnect(self) -> None:
        """A non-TTY consumer sees the live PAN line before the link drops."""
        self.assert_network_connection_is_visible_before_link_loss(PEER)

    def test_network_normalizes_lowercase_peer_before_visible_link_state(self) -> None:
        """A lowercase peer reaches BlueZ's uppercase object path and stays observable."""
        self.assert_network_connection_is_visible_before_link_loss(PEER.lower())

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
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
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
BUS_CONFIG = ROOT / "tests/host_tool/media/fixtures/private-bus.conf.in"


@dataclass(frozen=True)
class BluetoothTools:
    """The compiled client and controlled service peer."""

    client: Path
    service: Path


@pytest.fixture(scope="module")
def bluetooth_tools(tmp_path_factory: pytest.TempPathFactory) -> Iterator[BluetoothTools]:
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
        work = cleanup.enter_context(c_build_directory(tmp_path_factory, "fplinux-bluetooth"))
        tools = BluetoothTools(work / "fplinux-bluetooth", work / "fplinux-bluetooth-service")
        for source, output, name, extra_sources in (
            (
                CLIENT_SOURCE,
                tools.client,
                "compile FPLinux Bluetooth client",
                [
                    str(CLIENT_SOURCE.with_name("fplinux-bluetooth-opp.c")),
                    str(CLIENT_SOURCE.with_name("fplinux-bluetooth-pan.c")),
                    str(CLIENT_SOURCE.with_name("fplinux-bluetooth-common.c")),
                    str(ROOT / "lib/fplinux/fplinux-cli.c"),
                ],
            ),
            (SERVICE_SOURCE, tools.service, "compile FPLinux Bluetooth test service", []),
        ):
            compile_c(
                output,
                [
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{SHARED_INCLUDE}",
                    '-DFPLINUX_BLUETOOTH_DRIVER_DIR="platform-driver"',
                    *cflags,
                    str(source),
                    *extra_sources,
                    *libraries,
                ],
                name=name,
                check=True,
            )
        yield tools


@dataclass
class BluetoothContext:
    """Own one private bus and any service processes started for its scenario."""

    tools: BluetoothTools
    root: Path
    address: str
    cleanup: ExitStack
    fake: subprocess.Popen[str] | None = None

    @property
    def driver_directory(self) -> Path:
        """Return the synthetic device-discovery directory for this bus."""
        return self.root / "platform-driver"

    @property
    def service_directory(self) -> Path:
        """Keep the service's file lookup separate from the client's cwd."""
        return self.root / "service"

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
        ready = self.root / "service-ready"
        self.service_directory.mkdir(exist_ok=True)
        self.fake = subprocess.Popen(
            [str(self.tools.service), mode, str(ready)],
            env=self.child_environment(),
            cwd=self.service_directory,
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
            [str(self.tools.client), *arguments],
            name="run FPLinux Bluetooth client",
            timeout=5,
            env=self.child_environment(),
            cwd=self.root,
        )

    def prepare_driver_directory(self) -> Path:
        """Replace only device discovery with an owned temporary filesystem."""
        self.driver_directory.mkdir()
        self.cleanup.callback(shutil.rmtree, self.driver_directory)
        return self.driver_directory


@pytest.fixture
def bluetooth_case(bluetooth_tools: BluetoothTools) -> Iterator[BluetoothContext]:
    """Create and stop a private daemon separately for every scenario."""
    with ExitStack() as cleanup:
        temporary = cleanup.enter_context(tempfile.TemporaryDirectory())
        root = Path(temporary)
        configuration = root / "bus.conf"
        configuration.write_text(
            BUS_CONFIG.read_text(encoding="utf-8").format(directory=temporary), encoding="utf-8"
        )
        bus = subprocess.Popen(
            ["dbus-daemon", f"--config-file={configuration}", "--nofork", "--print-address=1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        cleanup.callback(BluetoothContext.stop_process, bus)
        if bus.stdout is None:
            pytest.fail("private D-Bus daemon stdout is not captured")
        readable, _, _ = select.select([bus.stdout], [], [], 3)
        address = bus.stdout.readline().strip() if readable else ""
        if not address:
            with suppress(ProcessLookupError):
                os.killpg(bus.pid, signal.SIGKILL)
            _, stderr = bus.communicate()
            pytest.fail(f"private D-Bus daemon did not publish an address:\n{stderr}")
        yield BluetoothContext(bluetooth_tools, root, address, cleanup)


class FplinuxBluetoothHostToolTests:
    """Exercise the client against a controlled D-Bus BlueZ/obexd fake."""

    def test_enable_writes_one_start_without_a_bus_service(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A bound controller receives the start request without BlueZ running."""
        directory = bluetooth_case.prepare_driver_directory()
        device = directory / "400a0000.bluetooth"
        device.mkdir()
        start = device / "start"
        start.write_bytes(b"")
        (directory / "uevent").write_text("", encoding="ascii")

        result = bluetooth_case.run_client("enable")

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
    def test_help_leaves_a_bound_controller_stopped(
        self, bluetooth_case: BluetoothContext, arguments: tuple[str, ...]
    ) -> None:
        """Root and subcommand help cannot write the synthetic start control."""
        directory = bluetooth_case.prepare_driver_directory()
        device = directory / "400a0000.bluetooth"
        device.mkdir()
        start = device / "start"
        start.write_bytes(b"")

        result = bluetooth_case.run_client(*arguments)

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
        self, bluetooth_case: BluetoothContext, arguments: tuple[str, ...]
    ) -> None:
        """Syntax and value errors are diagnosed with no running BlueZ service."""
        result = bluetooth_case.run_client(*arguments)

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--help") in (result.stderr)

    def test_enable_missing_board_is_optional_only_when_requested(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """Boot may omit an unsupported controller; explicit enable must explain it."""
        result = bluetooth_case.run_client("enable")
        assert (result.returncode) != (0)
        assert ("not configured") in (result.stderr)

        optional = bluetooth_case.run_client("enable", "--if-present")
        assert (optional.returncode) == (0), optional.stderr

    def test_enable_ambiguous_controller_does_not_start_either(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """An ambiguous discovery result cannot initialize an arbitrary controller."""
        directory = bluetooth_case.prepare_driver_directory()
        controls = []
        for name in ("controller-a", "controller-b"):
            device = directory / name
            device.mkdir()
            start = device / "start"
            start.write_bytes(b"")
            controls.append(start)

        result = bluetooth_case.run_client("enable", "--if-present")

        assert (result.returncode) != (0)
        assert ("multiple Bluetooth controllers") in (result.stderr)
        assert ([path.read_bytes() for path in controls]) == ([b"", b""])

    def test_send_accepts_terminal_signal_before_sendfile_reply(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A fast completed transfer is retained until its object path arrives."""
        payload = bluetooth_case.root / "payload"
        payload.write_bytes(b"")
        bluetooth_case.start_service("send-fast")

        result = bluetooth_case.run_client("send", PEER, str(payload))

        assert (result.returncode) == (0), result.stderr
        assert (result.stdout) == (f"sent {payload} to {PEER}\n")

    def test_send_relative_path_opens_the_clients_file_from_another_service_cwd(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """The service consumes the client's bytes despite a same-named local file."""
        payload = bluetooth_case.root / "payload"
        payload.write_bytes(b"selected by the client\n")
        bluetooth_case.service_directory.mkdir()
        (bluetooth_case.service_directory / "payload").write_bytes(b"service decoy\n")
        bluetooth_case.start_service("send-fast")

        result = bluetooth_case.run_client("send", PEER, "payload")

        assert result.returncode == 0, result.stderr
        assert (bluetooth_case.service_directory / "sent-bytes").read_bytes() == (
            b"selected by the client\n"
        )
        assert result.stdout == f"sent payload to {PEER}\n"

    @pytest.mark.parametrize("source_kind", ["missing", "directory"])
    def test_send_missing_or_nonregular_file_is_rejected_before_contacting_obexd(
        self, bluetooth_case: BluetoothContext, source_kind: str
    ) -> None:
        """Invalid file inputs are diagnosed even without an obexd service."""
        if source_kind == "directory":
            (bluetooth_case.root / "invalid-payload").mkdir()

        result = bluetooth_case.run_client("send", PEER, "invalid-payload")

        assert result.returncode == 1
        assert result.stdout == ""
        assert "invalid-payload" in result.stderr
        assert "D-Bus" not in result.stderr

    def test_receive_uses_name_before_authorization_and_writes_fixture_bytes(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """The advertised Name determines the authorized receive target."""
        destination = bluetooth_case.root / "received"
        destination.mkdir()
        bluetooth_case.start_service("receive")

        result = bluetooth_case.run_client("receive", PEER, str(destination), "3")

        received = destination / "incoming.txt"
        assert (result.returncode) == (0), result.stderr
        assert (received.read_bytes()) == (b"fixture")
        assert (f"received {received}\n") in (result.stdout)

    def test_receive_collision_preserves_existing_destination_bytes(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A completed incoming transfer never replaces an existing file."""
        destination = bluetooth_case.root / "received"
        destination.mkdir()
        existing = destination / "incoming.txt"
        existing.write_bytes(b"old-fixture")
        bluetooth_case.start_service("receive")

        result = bluetooth_case.run_client("receive", PEER, str(destination), "3")

        assert (result.returncode) != (0)
        assert (existing.read_bytes()) == (b"old-fixture")
        assert ("refusing to overwrite incoming.txt") in (result.stderr)

    def test_network_rejects_disconnect_before_connect_reply(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A PAN link already down at Connect completion has no usable output."""
        bluetooth_case.start_service("network-early")

        result = bluetooth_case.run_client("network", PEER)

        assert (result.returncode) != (0)
        assert (result.stdout) == ("")
        assert ("PAN disconnected before its interface could be used") in (result.stderr)

    def test_network_hides_interface_when_connected_snapshot_disappears(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A lost BlueZ device object cannot produce a usable PAN interface."""
        bluetooth_case.start_service("network-snapshot-missing")

        result = bluetooth_case.run_client("network", PEER)

        assert (result.returncode) != (0)
        assert (result.stdout) == ("")
        assert ("cannot read PAN Connected state") in (result.stderr)

    def assert_network_connection_is_visible_before_link_loss(
        self, bluetooth_case: BluetoothContext, peer: str
    ) -> None:
        """Observe one flushed PAN line while the client is still alive."""
        bluetooth_case.start_service("network-late")
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
                    assert bluetooth_case.fake is not None
                    os.kill(bluetooth_case.fake.pid, signal.SIGUSR1)
                    return
            pytest.fail("Bluetooth PAN client did not flush its connection line")

        result = run_process(
            [str(bluetooth_case.tools.client), "network", peer],
            name="run FPLinux Bluetooth PAN client",
            timeout=5,
            env=bluetooth_case.child_environment(),
            while_running=observe_connection,
        )

        assert (observed) == ([CONNECTED.replace(PEER, peer)])
        assert (result.returncode) == (0), result.stderr

    def test_network_flushes_connection_before_later_disconnect(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A non-TTY consumer sees the live PAN line before the link drops."""
        self.assert_network_connection_is_visible_before_link_loss(bluetooth_case, PEER)

    def test_network_normalizes_lowercase_peer_before_visible_link_state(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A lowercase peer reaches BlueZ's uppercase object path and stays observable."""
        self.assert_network_connection_is_visible_before_link_loss(bluetooth_case, PEER.lower())

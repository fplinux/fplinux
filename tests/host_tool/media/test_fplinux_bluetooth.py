# SPDX-License-Identifier: GPL-2.0-only
"""Private-bus and synthetic-sysfs checks for the FPLinux Bluetooth client."""

from __future__ import annotations

import os
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
from tests.process import read_process_line, run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

CLIENT_SOURCE = ROOT / "alpine/aports/fplinux-bluetooth/fplinux-bluetooth.c"
SHARED_INCLUDE = ROOT / "include/fplinux"
SERVICE_SOURCE = ROOT / "tests/host_tool/media/fplinux-bluetooth-service.c"
BLUEZ_FAKE = ROOT / "tests/host_tool/gui/support/bluez-fake.c"
SESSION_APORT = ROOT / "alpine/aports/fplinux-session"
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
    transfer: Path
    session: Path


@pytest.fixture(scope="module")
def bluetooth_tools(tmp_path_factory: pytest.TempPathFactory) -> Iterator[BluetoothTools]:
    """Build the real client and the test-owned fake through pkg-config."""
    with ExitStack() as cleanup:
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
        tools = BluetoothTools(
            work / "fplinux-bluetooth",
            work / "fplinux-bluetooth-service",
            work / "fplinux-bluetooth-transfer",
            work / "fplinux-bluetooth-session",
        )
        for source, output, name, extra_sources in (
            (
                CLIENT_SOURCE,
                tools.client,
                "compile FPLinux Bluetooth client",
                [
                    str(ROOT / "lib/fplinux-shared/fplinux-bluetooth-opp.c"),
                    str(CLIENT_SOURCE.with_name("fplinux-bluetooth-pan.c")),
                    str(CLIENT_SOURCE.with_name("fplinux-bluetooth-receive.c")),
                    str(ROOT / "lib/fplinux-shared/fplinux-bluetooth-common.c"),
                    str(ROOT / "lib/fplinux-shared/fplinux-bluez-adapter.c"),
                    str(ROOT / "lib/fplinux-shared/fplinux-cli.c"),
                ],
            ),
            (
                SERVICE_SOURCE,
                tools.service,
                "compile FPLinux Bluetooth test service",
                [f"-I{BLUEZ_FAKE.parent}", str(BLUEZ_FAKE)],
            ),
            (
                SERVICE_SOURCE.with_name("fplinux-bluetooth-transfer.c"),
                tools.transfer,
                "compile FPLinux Bluetooth transfer callbacks client",
                [
                    str(ROOT / "lib/fplinux-shared/fplinux-bluetooth-opp.c"),
                    str(ROOT / "lib/fplinux-shared/fplinux-bluetooth-common.c"),
                ],
            ),
            (
                SERVICE_SOURCE.with_name("fplinux-bluetooth-session.c"),
                tools.session,
                "compile the Bluetooth session worker boundary",
                [
                    f"-I{SESSION_APORT}",
                    f'-DFPLINUX_BLUETOOTH_RECEIVE_EXECUTABLE="{tools.client}"',
                    str(SESSION_APORT / "session-bluetooth.c"),
                ],
            ),
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
                    '-DFPLINUX_BLUETOOTH_RECEIVE_SOCKET="receive.sock"',
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
    """Own one isolated file tree, bus address and any service processes."""

    tools: BluetoothTools
    root: Path
    address: str
    cleanup: ExitStack
    fake: subprocess.Popen[str] | None = None

    @property
    def driver_directory(self) -> Path:
        """Return the synthetic device-discovery directory for this scenario."""
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
        """Pass the isolated address only to the client and its fake service."""
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
            with suppress(FileNotFoundError):
                if ready.read_bytes() == b"ready\n":
                    return
            if self.fake.poll() is not None:
                stdout, stderr = self.fake.communicate()
                pytest.fail(f"Bluetooth fake exited before readiness:\n{stdout}\n{stderr}")
            time.sleep(0.01)
        pytest.fail("Bluetooth fake did not publish readiness")

    def run_client(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        """Invoke the production client with this test's isolated bus address."""
        return run_process(
            [str(self.tools.client), *arguments],
            name="run FPLinux Bluetooth client",
            timeout=5,
            env=self.child_environment(),
            cwd=self.root,
        )

    def run_transfer(
        self, command: str, path: Path, policy: str, *, cancel_on: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        """Invoke the shared blocking API with test-owned consent callbacks."""
        environment = self.child_environment()
        if cancel_on is not None:
            environment["FPLINUX_TEST_OBEX_SESSION_REQUEST"] = str(cancel_on)
        return run_process(
            [str(self.tools.transfer), command, str(path), policy],
            name="run FPLinux Bluetooth transfer callbacks client",
            timeout=5,
            env=environment,
            cwd=self.root,
        )

    def prepare_driver_directory(self) -> Path:
        """Replace only device discovery with an owned temporary filesystem."""
        self.driver_directory.mkdir()
        self.cleanup.callback(shutil.rmtree, self.driver_directory)
        return self.driver_directory

    def start_session(self) -> subprocess.Popen[str]:
        """Start the real receive worker through the session's child boundary."""
        session = subprocess.Popen(
            [str(self.tools.session), str(self.root)],
            env=self.child_environment(),
            cwd=self.root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        self.cleanup.callback(self.stop_process, session)
        self.session_event(session, "ready")
        return session

    @staticmethod
    def session_event(session: subprocess.Popen[str], state: str) -> str:
        """Wait for the requested event kind through the production session socket."""
        assert session.stdout is not None
        deadline = time.monotonic() + 3
        while True:
            line = read_process_line(session.stdout, deadline=deadline, name=state).strip()
            if line.split(maxsplit=1)[0] == state:
                return line

    def push_result(self, expected: str) -> None:
        """Wait for the peer's authorization result and clear its marker."""
        marker = self.service_directory / "push-result"
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            with suppress(FileNotFoundError):
                observed = marker.read_text(encoding="ascii")
                if observed:
                    assert observed == f"{expected}\n"
                    marker.unlink()
                    return
            time.sleep(0.01)
        pytest.fail(f"expected peer authorization result {expected}")


@pytest.fixture
def bluetooth_without_bus(bluetooth_tools: BluetoothTools) -> Iterator[BluetoothContext]:
    """Reject any bus connection through an absent socket in an owned directory."""
    with ExitStack() as cleanup:
        temporary = cleanup.enter_context(tempfile.TemporaryDirectory())
        root = Path(temporary)
        address = f"unix:path={root / 'absent-bus'}"
        yield BluetoothContext(bluetooth_tools, root, address, cleanup)


@pytest.fixture
def bluetooth_case(bluetooth_tools: BluetoothTools) -> Iterator[BluetoothContext]:
    """Create and stop a private daemon separately for every scenario."""
    with ExitStack() as cleanup:
        if shutil.which("dbus-daemon") is None:
            message = "dbus-daemon is required for Bluetooth private-bus tests"
            pytest.skip(message)
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
        address = read_process_line(
            bus.stdout, deadline=time.monotonic() + 3, name="private D-Bus daemon address"
        ).strip()
        assert address, "private D-Bus daemon published an empty address"
        yield BluetoothContext(bluetooth_tools, root, address, cleanup)


class FplinuxBluetoothHostToolTests:
    """Exercise local admission and controlled D-Bus BlueZ/obexd behavior."""

    def test_enable_writes_one_start_without_a_bus_service(
        self, bluetooth_without_bus: BluetoothContext
    ) -> None:
        """A bound controller receives the start request without BlueZ running."""
        directory = bluetooth_without_bus.prepare_driver_directory()
        device = directory / "400a0000.bluetooth"
        device.mkdir()
        start = device / "start"
        start.write_bytes(b"")
        (directory / "uevent").write_text("", encoding="ascii")

        result = bluetooth_without_bus.run_client("enable")

        assert (result.returncode) == (0), result.stderr
        assert (start.read_bytes()) == (b"1\n")
        assert (result.stdout) == (
            "Bluetooth interfaces ready; use bluetoothctl for adapter power\n"
        )

    def test_session_receive_validates_exact_consent_hands_off_agent_and_keeps_both_files(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        bluetooth_case.start_service("receive-session")
        session = bluetooth_case.start_session()
        assert session.stdin is not None
        request = bluetooth_case.service_directory / "push"
        destination = bluetooth_case.root / "Files/Bluetooth"

        request.touch()
        first = bluetooth_case.session_event(session, "offer").split()
        assert first[2:] == ["PairedPhone", "incoming.txt", "7", "1"]
        busy = bluetooth_case.run_client("receive", PEER, str(bluetooth_case.root), "1")
        assert busy.returncode == 1
        assert "Bluetooth receive is busy" in busy.stderr
        session.stdin.write(f"{int(first[1]) + 1} 1\n{first[1]} 0\n")
        session.stdin.flush()
        assert bluetooth_case.session_event(session, "cleared") == f"cleared {first[1]}"
        bluetooth_case.push_result("rejected")
        assert not destination.exists()

        diagnostic = bluetooth_case.run_client("receive", PEER, str(bluetooth_case.root), "1")
        assert diagnostic.returncode == 1
        assert "receive window expired" in diagnostic.stderr
        bluetooth_case.session_event(session, "busy")
        bluetooth_case.session_event(session, "ready")

        for filename in ("incoming.txt", "incoming (1).txt"):
            request.touch()
            offer = bluetooth_case.session_event(session, "offer").split()
            assert offer[1] != first[1]
            session.stdin.write(f"{offer[1]} 1\n")
            session.stdin.flush()
            assert bluetooth_case.session_event(session, "complete") == (
                f"complete {offer[1]} {destination / filename}"
            )
            bluetooth_case.push_result("written")

        assert sorted(entry.name for entry in destination.iterdir()) == [
            "incoming (1).txt",
            "incoming.txt",
        ]
        assert (destination / "incoming.txt").read_bytes() == b"fixture"
        assert (destination / "incoming (1).txt").read_bytes() == b"fixture"

    @pytest.mark.parametrize("boundary", ["wrong-local", "unpaired"])
    def test_session_receive_rejects_a_source_outside_the_powered_pair(
        self, bluetooth_case: BluetoothContext, boundary: str
    ) -> None:
        bluetooth_case.service_directory.mkdir()
        (bluetooth_case.service_directory / boundary).touch()
        bluetooth_case.start_service("receive-session")
        bluetooth_case.start_session()

        (bluetooth_case.service_directory / "push").touch()
        bluetooth_case.push_result("rejected")

        assert not (bluetooth_case.root / "Files").exists()

    def test_session_receive_power_off_clears_an_offer_and_truncated_transfer_leaves_no_file(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        bluetooth_case.start_service("receive-session")
        session = bluetooth_case.start_session()
        assert session.stdin is not None
        directory = bluetooth_case.service_directory
        (directory / "push").touch()
        first = bluetooth_case.session_event(session, "offer").split()[1]

        (directory / "power-off").touch()
        bluetooth_case.session_event(session, "off")
        assert bluetooth_case.session_event(session, "cleared") == f"cleared {first}"
        bluetooth_case.push_result("rejected")
        assert not (bluetooth_case.root / "Files").exists()

        (directory / "power-on").touch()
        bluetooth_case.session_event(session, "ready")
        (directory / "truncate-push").touch()
        (directory / "push").touch()
        second = bluetooth_case.session_event(session, "offer").split()[1]
        assert second != first
        session.stdin.write(f"{second} 1\n")
        session.stdin.flush()
        assert bluetooth_case.session_event(session, "failed") == f"failed {second}"
        bluetooth_case.push_result("written")

        assert list((bluetooth_case.root / "Files/Bluetooth").iterdir()) == []

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
        self, bluetooth_without_bus: BluetoothContext, arguments: tuple[str, ...]
    ) -> None:
        """Root and subcommand help cannot write the synthetic start control."""
        directory = bluetooth_without_bus.prepare_driver_directory()
        device = directory / "400a0000.bluetooth"
        device.mkdir()
        start = device / "start"
        start.write_bytes(b"")

        result = bluetooth_without_bus.run_client(*arguments)

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
        self, bluetooth_without_bus: BluetoothContext, arguments: tuple[str, ...]
    ) -> None:
        """Syntax and value errors are diagnosed with no running BlueZ service."""
        result = bluetooth_without_bus.run_client(*arguments)

        assert (result.returncode) == (2), result.stderr
        assert (result.stdout) == ("")
        assert ("--help") in (result.stderr)

    def test_enable_missing_board_is_optional_only_when_requested(
        self, bluetooth_without_bus: BluetoothContext
    ) -> None:
        """Boot may omit an unsupported controller; explicit enable must explain it."""
        result = bluetooth_without_bus.run_client("enable")
        assert (result.returncode) != (0)
        assert ("not configured") in (result.stderr)

        optional = bluetooth_without_bus.run_client("enable", "--if-present")
        assert (optional.returncode) == (0), optional.stderr

    def test_enable_ambiguous_controller_does_not_start_either(
        self, bluetooth_without_bus: BluetoothContext
    ) -> None:
        """An ambiguous discovery result cannot initialize an arbitrary controller."""
        directory = bluetooth_without_bus.prepare_driver_directory()
        controls = []
        for name in ("controller-a", "controller-b"):
            device = directory / name
            device.mkdir()
            start = device / "start"
            start.write_bytes(b"")
            controls.append(start)

        result = bluetooth_without_bus.run_client("enable", "--if-present")

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
        self, bluetooth_without_bus: BluetoothContext, source_kind: str
    ) -> None:
        """Invalid file inputs are diagnosed even without an obexd service."""
        if source_kind == "directory":
            (bluetooth_without_bus.root / "invalid-payload").mkdir()

        result = bluetooth_without_bus.run_client("send", PEER, "invalid-payload")

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

    def test_shared_send_reports_cumulative_bytes_from_transfer_signals(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """The shared caller observes transferred bytes and a known source size."""
        payload = bluetooth_case.root / "payload"
        payload.write_bytes(b"1234567")
        bluetooth_case.start_service("send-fast")

        result = bluetooth_case.run_transfer("send", payload, "accept")

        assert result.returncode == 0, result.stderr
        assert f"{payload} 7 7 1\n" in result.stdout
        assert (bluetooth_case.service_directory / "sent-bytes").read_bytes() == b"1234567"

    def test_shared_send_preserves_early_completion_across_an_unrelated_transfer_signal(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """Another session's status cannot replace this sender's early completion."""
        payload = bluetooth_case.root / "payload"
        payload.write_bytes(b"1234567")
        bluetooth_case.start_service("send-fast-unrelated")

        result = bluetooth_case.run_transfer("send", payload, "accept")

        assert result.returncode == 0, result.stderr
        assert "result 2 0 0 " in result.stdout

    def test_shared_send_cancels_a_pending_session_call_promptly(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """A missing method reply cannot delay cancellation by its 15 second timeout."""
        payload = bluetooth_case.root / "payload"
        payload.write_bytes(b"1234567")
        bluetooth_case.start_service("send-stall")

        requested = bluetooth_case.service_directory / "session-requested"
        result = bluetooth_case.run_transfer("send", payload, "cancel-wait", cancel_on=requested)
        completed = time.monotonic()

        assert result.returncode == 1
        assert "result 1 " in result.stdout
        assert "message interrupted\n" in result.stdout
        assert 0 <= completed - int(requested.read_bytes()) / 1000 < 1.5
        assert not (bluetooth_case.service_directory / "sent-bytes").exists()

    @pytest.mark.parametrize("policy", ["reject", "cancel-offer"])
    def test_shared_receive_declined_offer_never_publishes_or_leaves_staging(
        self, bluetooth_case: BluetoothContext, policy: str
    ) -> None:
        """Consent is explicit and cancellation while authorizing creates no file."""
        destination = bluetooth_case.root / "received"
        destination.mkdir()
        bluetooth_case.start_service("receive")

        result = bluetooth_case.run_transfer("receive", destination, policy)

        assert result.returncode == 1
        assert f"offer {PEER} incoming.txt 7 1\n" in result.stdout
        assert list(destination.iterdir()) == []

    def test_shared_receive_cancel_after_progress_removes_partial_file(
        self, bluetooth_case: BluetoothContext
    ) -> None:
        """Cancel reaches the daemon and removes incomplete private staging bytes."""
        destination = bluetooth_case.root / "received"
        destination.mkdir()
        bluetooth_case.start_service("receive-slow")

        result = bluetooth_case.run_transfer("receive", destination, "cancel-progress")

        assert result.returncode == 1
        assert "incoming.txt 3 7 1\n" in result.stdout
        assert "result 1 " in result.stdout
        assert list(destination.iterdir()) == []
        assert (bluetooth_case.service_directory / "cancelled").read_bytes() == b"cancelled\n"

    @pytest.mark.parametrize(
        ("service_mode", "expected_message"),
        [
            ("receive-owner-loss", "stopped while transfer was active"),
            ("receive-agent-cancel", "transfer ended with status error"),
        ],
    )
    def test_shared_receive_services_daemon_events_while_consent_is_pending(
        self, bluetooth_case: BluetoothContext, service_mode: str, expected_message: str
    ) -> None:
        """A pending user decision cannot hide daemon disappearance or Agent.Cancel."""
        destination = bluetooth_case.root / "received"
        destination.mkdir()
        bluetooth_case.start_service(service_mode)

        result = bluetooth_case.run_transfer("receive", destination, "pending")

        assert result.returncode == 1
        assert f"offer {PEER} incoming.txt 7 1\n" in result.stdout
        assert expected_message in result.stdout
        assert list(destination.iterdir()) == []

    @pytest.mark.parametrize(
        ("policy", "expected_name", "existing_bytes"),
        [("keep-both", "incoming (1).txt", b"original"), ("replace", "incoming.txt", b"fixture")],
    )
    def test_shared_receive_conflict_choice_controls_real_publication(
        self,
        bluetooth_case: BluetoothContext,
        policy: str,
        expected_name: str,
        existing_bytes: bytes,
    ) -> None:
        """Keep both preserves old bytes; explicit replacement publishes new bytes."""
        destination = bluetooth_case.root / "received"
        destination.mkdir()
        (destination / "incoming.txt").write_bytes(b"original")
        bluetooth_case.start_service("receive")

        result = bluetooth_case.run_transfer("receive", destination, policy)

        assert result.returncode == 0, result.stderr
        assert "conflict incoming.txt\n" in result.stdout
        assert (destination / expected_name).read_bytes() == b"fixture"
        assert (destination / "incoming.txt").read_bytes() == existing_bytes
        assert not list(destination.glob(".fplinux-bluetooth-*"))

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
            observed.append(
                read_process_line(process.stdout, deadline=deadline, name="Bluetooth PAN client")
            )
            assert process.poll() is None
            assert bluetooth_case.fake is not None
            os.kill(bluetooth_case.fake.pid, signal.SIGUSR1)

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

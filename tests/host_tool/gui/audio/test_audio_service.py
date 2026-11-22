# SPDX-License-Identifier: GPL-2.0-only
"""Production daemon, authenticated processes and rings against a paced device peer."""

from __future__ import annotations

import array
import errno
import os
import socket
import subprocess
import time
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests import ROOT, audio_wire
from tests.c_harness import compile_c
from tests.process import connect_unix_seqpacket, read_process_line, run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

APORT = ROOT / "alpine/aports/fplinux-audiod"
HERE = ROOT / "tests/host_tool/gui/audio"


@pytest.fixture(scope="module")
def programs(tmp_path_factory: pytest.TempPathFactory) -> tuple[Path, Path]:
    directory = tmp_path_factory.mktemp("audio-service")
    daemon = directory / "audiod"
    peer = directory / "stream-peer"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread"]
    includes = [f"-I{ROOT / 'include/fplinux'}", f"-I{APORT}"]
    compile_c(
        daemon,
        [
            *flags,
            *includes,
            str(APORT / "audiod.c"),
            str(APORT / "audio-streams.c"),
            str(APORT / "audio-mixer.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-audio-control.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-audio-radio.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-audio-status.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-cli.c"),
            str(HERE / "audio-device-peer.c"),
        ],
        name="compile production audio daemon with paced device peer",
    )
    compile_c(
        peer,
        [*flags, *includes, str(HERE / "audio-stream-peer.c")],
        name="compile real-process audio stream peer",
    )
    return daemon, peer


class Stream:
    """Own one authenticated producer process and its command channel."""

    def __init__(self, program: Path, path: Path, cleanup: ExitStack) -> None:
        """Start the producer and receive its actual peer PID."""
        self.process = subprocess.Popen(
            [str(program), str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        cleanup.callback(self.close)
        assert self.process.stdout is not None
        line = read_process_line(
            self.process.stdout, deadline=time.monotonic() + 3, name="audio stream peer PID"
        )
        self.pid = int(line)

    def command(self, command: str) -> str:
        """Send one producer command and wait for its bounded reply."""
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return read_process_line(
            self.process.stdout, deadline=time.monotonic() + 3, name=f"audio stream peer {command}"
        ).removesuffix("\n")

    def state(self) -> tuple[int, ...]:
        """Read the producer's ring counters and applied policy state."""
        return tuple(int(value) for value in self.command("STATE").split())

    def close(self) -> None:
        """Close the producer channel and reap the process."""
        try:
            _, stderr = self.process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate(timeout=3)
            raise
        assert self.process.returncode == 0, stderr


class Service:
    """Run audiod with real sockets and a paced replacement device."""

    def __init__(
        self,
        directory: Path,
        programs: tuple[Path, Path],
        cleanup: ExitStack,
        *,
        headphones_only: bool = False,
        options: tuple[str, ...] = (),
    ) -> None:
        """Start the daemon with caller-owned sockets and PCM evidence."""
        self.directory = directory
        self.peer = programs[1]
        self.cleanup = cleanup
        self.manager: socket.socket | None = None
        self.activities: list[tuple[int, int, str]] = []
        environment = {
            **os.environ,
            "FPLINUX_TEST_PCM": str(directory / "samples"),
            "FPLINUX_TEST_EVENTS": str(directory / "events"),
            "FPLINUX_TEST_JACK": str(directory / "jack"),
        }
        if headphones_only:
            environment["FPLINUX_TEST_HEADPHONES_ONLY"] = "1"
        else:
            environment.pop("FPLINUX_TEST_HEADPHONES_ONLY", None)
        self.process = subprocess.Popen(
            [str(programs[0]), "--dir", str(directory), *options],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        cleanup.callback(self.close)
        self.connect()

    def socket(self, name: str) -> socket.socket:
        """Connect a bounded client to one of the service endpoints."""
        return connect_unix_seqpacket(self.directory / name)

    def connect(self, *, ready: bool = True) -> None:
        """Connect the manager and optionally commit its initial replay."""
        self.manager = self.socket("control.sock")
        self.cleanup.callback(self.manager.close)
        self.volume("MEDIA", 10)
        self.volume("ALARM", 10)
        if ready:
            self.control(audio_wire.SYNC)

    def control(self, command: bytes) -> None:
        """Send one policy command over the current manager connection."""
        assert self.manager is not None
        self.manager.send(command)

    def volume(self, kind: str, level: int) -> None:
        """Set a class volume and require its exact applied ACK."""
        self.control(audio_wire.set_volume(kind, level))
        self.expect_reply(audio_wire.volume_applied(kind, level))

    def expect_reply(self, expected: bytes) -> None:
        """Route validated activity notifications without mistaking them for ACKs."""
        assert self.manager is not None
        while True:
            packet = self.manager.recv(256)
            if not audio_wire.is_activity(packet):
                assert packet == expected
                return
            self.activities.append(audio_wire.activity(packet))

    def announce(self, stream: Stream, *, background: bool = False, clock: bool = False) -> None:
        """Authorize the producer's credentials and assigned role."""
        self.control(
            audio_wire.clock(stream.pid, os.getuid())
            if clock
            else audio_wire.app(stream.pid, os.getuid(), background=background)
        )

    def stream(self, *, background: bool = False, clock: bool = False) -> Stream:
        """Open a real producer stream after announcing its credentials."""
        stream = Stream(self.peer, self.directory / "stream.sock", self.cleanup)
        self.announce(stream, background=background, clock=clock)
        assert stream.command("OPEN") == "OK"
        return stream

    def wait_state(self, stream: Stream, *, state: int, reason: int) -> tuple[int, ...]:
        """Wait for the service to publish the expected state and reason."""
        deadline = time.monotonic() + 3
        while True:
            result = stream.state()
            if result[2:] == (state, reason):
                return result
            assert time.monotonic() < deadline, result
            time.sleep(0.005)

    def wait_read(self, stream: Stream, minimum: int) -> tuple[int, ...]:
        """Wait for the service to consume the required number of frames."""
        deadline = time.monotonic() + 3
        while True:
            result = stream.state()
            if result[0] >= minimum:
                return result
            assert time.monotonic() < deadline, result
            time.sleep(0.005)

    def wait_device_events(self, *expected: str) -> None:
        """Wait for the device peer to finish recording an ordered event prefix."""
        path = self.directory / "events"
        deadline = time.monotonic() + 3
        while True:
            events = path.read_text().splitlines()
            if len(events) >= len(expected):
                assert tuple(events[: len(expected)]) == expected
                return
            assert time.monotonic() < deadline, events
            time.sleep(0.005)

    def change_jack(self, change: bytes) -> None:
        """Send a jack event when the device peer has opened the FIFO."""
        path = self.directory / "jack"
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            try:
                descriptor = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
            except OSError as error:
                if error.errno not in {errno.ENOENT, errno.ENXIO}:
                    raise
            else:
                try:
                    written = os.write(descriptor, change)
                finally:
                    os.close(descriptor)
                assert written == len(change)
                return
            assert self.process.poll() is None, "audio daemon exited before reading jack events"
            time.sleep(0.005)
        message = "audio daemon did not open the jack FIFO"
        raise AssertionError(message)

    def close(self) -> None:
        """Stop and reap the daemon."""
        self.process.terminate()
        try:
            output = self.process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate(timeout=3)
            raise
        assert self.process.returncode == 0, output


@pytest.fixture
def service_directory() -> Iterator[Path]:
    """Keep Unix socket paths below the daemon's directory-length limit."""
    with TemporaryDirectory(prefix="fpa-") as scratch:
        yield Path(scratch)


@pytest.fixture
def service(service_directory: Path, programs: tuple[Path, Path]) -> Iterator[Service]:
    with ExitStack() as cleanup:
        yield Service(service_directory, programs, cleanup)


@pytest.fixture
def headphones_only_service(
    service_directory: Path, programs: tuple[Path, Path]
) -> Iterator[Service]:
    with ExitStack() as cleanup:
        yield Service(service_directory, programs, cleanup, headphones_only=True)


def test_board_options_select_mix_rate_and_fixed_speaker(
    service_directory: Path, programs: tuple[Path, Path]
) -> None:
    """The daemon reports its configured rate and a speaker without a mixer switch."""
    with ExitStack() as cleanup:
        configured = Service(
            service_directory,
            programs,
            cleanup,
            headphones_only=True,
            options=("--rate", "44100", "--fixed-speaker"),
        )
        with configured.socket("status.sock") as diagnostic:
            state = audio_wire.status(diagnostic)
        assert state["rate"] == 44100
        assert state["speaker"] == 1


def test_foreground_pcm_interrupts_background_and_user_pause_survives_alarm(
    service: Service,
) -> None:
    background = service.stream(background=True)
    foreground = service.stream()
    clock = service.stream(clock=True)
    service.control(audio_wire.eligible(foreground.pid, enabled=True))
    assert background.command("WRITE 12000 12000") == "OK"
    service.wait_read(background, 480)
    service.wait_state(background, state=0, reason=0)
    assert foreground.command("WRITE 9600 24000") == "OK"
    paused = service.wait_state(background, state=1, reason=3)
    time.sleep(0.025)
    assert background.state()[0] == paused[0]
    service.control(audio_wire.eligible(clock.pid, enabled=True))
    assert clock.command("WRITE 4800 30000") == "OK"
    service.wait_state(foreground, state=1, reason=4)
    assert background.command("PAUSE 1") == "OK"
    service.wait_state(background, state=1, reason=1)
    service.wait_read(clock, 4800)
    service.wait_read(foreground, 9600)
    assert background.state()[0] == paused[0]
    assert background.command("PAUSE 0") == "OK"
    service.wait_read(background, 12000)


def test_media_mute_does_not_mute_alarm_and_gain_applies_once(service: Service) -> None:
    service.volume("MEDIA", 0)
    media = service.stream(background=True)
    assert media.command("WRITE 2400 16000") == "OK"
    service.wait_read(media, 2400)
    service.volume("ALARM", 5)
    clock = service.stream(clock=True)
    service.control(audio_wire.eligible(clock.pid, enabled=True))
    assert clock.command("WRITE 2400 20000") == "OK"
    service.wait_read(clock, 2400)
    service.volume("ALARM", 0)
    assert clock.command("WRITE 2400 20000") == "OK"
    service.wait_read(clock, 4800)
    samples = array.array("h", (service.directory / "samples").read_bytes())
    assert 3556 in samples
    assert max(samples) == 3556
    assert list(samples[-960:]) == [0] * 960


@pytest.mark.parametrize("kind", ["MEDIA", "ALARM"])
@pytest.mark.parametrize(
    ("level", "peak"),
    [
        (0, 0),
        (1, 893),
        (2, 1262),
        (3, 1782),
        (4, 2517),
        (5, 3556),
        (6, 5023),
        (7, 7096),
        (8, 10023),
        (9, 14158),
        (10, 20000),
    ],
)
def test_class_gain_uses_one_three_db_curve(
    service: Service, kind: str, level: int, peak: int
) -> None:
    service.volume(kind, level)
    stream = service.stream(background=kind == "MEDIA", clock=kind == "ALARM")
    if kind == "ALARM":
        service.control(audio_wire.eligible(stream.pid, enabled=True))
    assert stream.command("WRITE 2400 20000") == "OK"
    service.wait_read(stream, 2400)
    samples = array.array("h", (service.directory / "samples").read_bytes())
    assert max(samples) == peak


def test_manager_disconnect_revokes_old_grant_and_diagnostic_does_not_replace_it(
    service: Service,
) -> None:
    old = service.stream(background=True)
    current = service.stream()
    assert old.command("WRITE 12000 10000") == "OK"
    service.wait_read(old, 480)
    with service.socket("status.sock") as diagnostic:
        state = audio_wire.status(diagnostic)
        assert (state["media"], state["alarm"], state["rate"]) == (10, 10, 48000)
    with service.socket("control.sock") as other:
        assert other.recv(32) == b""
    service.volume("MEDIA", 7)
    assert service.manager is not None
    service.manager.close()
    paused = service.wait_state(old, state=1, reason=2)
    service.connect(ready=False)
    service.volume("MEDIA", 3)
    service.announce(old)
    service.announce(current, background=True)
    assert current.command("WRITE 4800 20000") == "OK"
    service.wait_state(current, state=1, reason=2)
    assert current.state()[0] == 0
    service.control(audio_wire.SYNC)
    service.wait_read(current, 4800)
    assert old.state()[0] == paused[0]
    service.wait_state(old, state=1, reason=2)


def test_fm_closes_pcm_before_claim_and_alarm_releases_then_restores_radio(
    service: Service,
) -> None:
    media = service.stream(background=True)
    assert media.command("WRITE 12000 10000") == "OK"
    service.wait_read(media, 480)
    with service.socket("radio.sock") as radio:
        radio.send(audio_wire.radio_play(999))
        assert radio.recv(32) == audio_wire.RADIO_READY
        service.wait_state(media, state=1, reason=6)
        clock = service.stream(clock=True)
        service.control(audio_wire.eligible(clock.pid, enabled=True))
        assert clock.command("WRITE 2400 18000") == "OK"
        service.wait_read(clock, 2400)
        service.wait_state(media, state=1, reason=6)
    service.wait_read(media, 12000)
    events = (service.directory / "events").read_text().splitlines()
    assert events[:4] == ["PCM OPEN", "TUNE", "PCM CLOSE", "FM ON"]
    assert events[4:8] == ["FM OFF", "PCM OPEN", "PCM CLOSE", "FM ON"]
    assert events[8:10] == ["FM OFF", "PCM OPEN"]


def test_headphone_loss_pauses_media_before_speaker_fallback_and_alarm_still_plays(
    service: Service,
) -> None:
    media = service.stream(background=True)
    assert media.command("WRITE 12000 10000") == "OK"
    service.wait_read(media, 480)
    service.change_jack(b"U")
    paused = service.wait_state(media, state=1, reason=5)
    service.wait_device_events("PCM OPEN", "PCM CLOSE", "ROUTE SPEAKER")
    clock = service.stream(clock=True)
    service.control(audio_wire.eligible(clock.pid, enabled=True))
    assert clock.command("WRITE 2400 18000") == "OK"
    service.wait_read(clock, 2400)
    assert media.state()[0] == paused[0]
    assert media.command("PAUSE 0") == "OK"
    service.wait_read(media, 12000)


def test_unconfirmed_clock_ring_expires_after_reconnect_and_silent_alarm_resumes_media(
    service: Service,
) -> None:
    media = service.stream(background=True)
    clock = service.stream(clock=True)
    service.volume("ALARM", 0)
    service.control(audio_wire.eligible(clock.pid, enabled=True))
    assert clock.command("WRITE 4800 20000") == "OK"
    assert media.command("WRITE 12000 10000") == "OK"
    service.wait_state(media, state=1, reason=4)
    service.wait_read(clock, 4800)
    service.wait_read(media, 480)
    samples = array.array("h", (service.directory / "samples").read_bytes())
    assert list(samples[:9600]) == [0] * 9600
    assert service.manager is not None
    service.manager.close()
    service.wait_state(media, state=1, reason=2)
    service.connect(ready=False)
    service.announce(media, background=True)
    service.control(audio_wire.SYNC)
    service.wait_read(media, 12000)
    service.wait_state(clock, state=3, reason=2)


def test_selected_missing_headphones_hold_new_media_until_connection_while_alarm_drains(
    service: Service,
) -> None:
    service.control(audio_wire.set_output(2))
    assert service.manager is not None
    service.expect_reply(audio_wire.output_applied(2))
    service.change_jack(b"U")
    media = service.stream(background=True)
    assert media.command("WRITE 12000 10000") == "OK"
    service.wait_state(media, state=1, reason=5)
    assert media.state()[0] == 0
    clock = service.stream(clock=True)
    service.control(audio_wire.eligible(clock.pid, enabled=True))
    assert clock.command("WRITE 2400 18000") == "OK"
    service.wait_read(clock, 2400)
    assert media.state()[0] == 0
    service.change_jack(b"I")
    service.wait_read(media, 12000)
    with service.socket("status.sock") as diagnostic:
        state = audio_wire.status(diagnostic)
        assert (state["output"], state["applied"]) == (2, 2)


def test_auto_route_with_no_speaker_and_known_missing_headphones_cannot_claim_playback(
    headphones_only_service: Service,
) -> None:
    service = headphones_only_service
    media = service.stream(background=True)
    assert media.command("WRITE 12000 10000") == "OK"
    service.wait_state(media, state=1, reason=5)
    clock = service.stream(clock=True)
    service.control(audio_wire.eligible(clock.pid, enabled=True))
    assert clock.command("WRITE 2400 18000") == "OK"
    service.wait_state(clock, state=2, reason=5)
    assert media.state()[0] == 0
    assert clock.state()[0] == 0
    assert (service.directory / "events").read_text() == ""


def test_status_cli_reads_running_state_without_opening_devices_or_replacing_manager(
    service: Service, programs: tuple[Path, Path]
) -> None:
    result = run_process(
        [str(programs[0]), "--dir", str(service.directory), "--status"],
        name="read live audiod state through its CLI",
        timeout=3,
        check=True,
    )
    assert result.stdout.startswith("STATE media=10 alarm=10 rate=48000 ")
    assert result.stdout.endswith("\n")
    assert (service.directory / "events").read_text() == ""
    service.volume("MEDIA", 6)


STATUS_PACKET = bytes.fromhex(
    "01 07 02 44 ac 00 00 01 02 00 01 01 01 01 00 01 00 01 ff ff ff ff ff ff ff ff 13 00 00 00"
)


@pytest.mark.parametrize(
    ("packet", "error"),
    [
        (STATUS_PACKET, None),
        (None, errno.ETIMEDOUT),
        (b"", errno.EPROTO),
        (STATUS_PACKET[:-1], errno.EPROTO),
        (STATUS_PACKET + bytes(32), errno.EPROTO),
        (b"\x02" + STATUS_PACKET[1:], errno.EPROTO),
        (b"\x01\x0b" + STATUS_PACKET[2:], errno.EPROTO),
        (STATUS_PACKET[:3] + bytes(4) + STATUS_PACKET[7:], errno.EPROTO),
        (STATUS_PACKET[:8] + b"\x03" + STATUS_PACKET[9:], errno.EPROTO),
        (STATUS_PACKET[:9] + b"\x02" + STATUS_PACKET[10:], errno.EPROTO),
        (STATUS_PACKET[:-1] + b"\x80", errno.EPROTO),
    ],
    ids=[
        "valid",
        "timeout",
        "closed",
        "short",
        "truncated",
        "wrong-type",
        "level",
        "rate",
        "route",
        "flag",
        "errno",
    ],
)
def test_status_cli_requires_one_valid_snapshot(
    service_directory: Path,
    programs: tuple[Path, Path],
    packet: bytes | None,
    error: int | None,
) -> None:
    with ExitStack() as cleanup:
        listener = cleanup.enter_context(socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET))
        listener.bind(str(service_directory / "status.sock"))
        listener.listen()
        listener.settimeout(2)

        def send_snapshot(_process: subprocess.Popen[str], _deadline: float) -> None:
            connection, _ = listener.accept()
            cleanup.callback(connection.close)
            if packet is not None:
                if packet:
                    connection.sendall(packet)
                connection.close()

        result = run_process(
            [str(programs[0]), "--dir", str(service_directory), "--status"],
            name="read an independent audiod snapshot through the status CLI",
            timeout=3,
            while_running=send_snapshot,
        )
    if error is not None:
        assert result.returncode == 1
        assert result.stdout == ""
        assert result.stderr == f"fplinux-audiod: status: {os.strerror(error)}\n"
    else:
        assert result.returncode == 0
        assert result.stderr == ""
        assert result.stdout == (
            "STATE media=7 alarm=2 rate=44100 output=1 applied=2 speaker=0 headphones=1"
            " jack=1 plugged=1 fm=1 fm_active=0 clock=1 foreground=0 pcm=1"
            " clipped=18446744073709551615 error=19\n"
        )


def test_status_cli_help_returns_before_device_initialization(programs: tuple[Path, Path]) -> None:
    result = run_process(
        [str(programs[0]), "--status", "--help"],
        name="show audiod CLI help without hardware",
        timeout=3,
        check=True,
    )
    assert "--status" in result.stdout
    assert "serving" not in result.stdout


@pytest.mark.parametrize("radio", [False, True], ids=["pcm", "fm"])
def test_explicit_speaker_keeps_playing_when_unused_headphone_antenna_is_removed(
    service: Service, *, radio: bool
) -> None:
    service.control(audio_wire.set_output(1))
    assert service.manager is not None
    service.expect_reply(audio_wire.output_applied(1))
    media = service.stream(background=True)
    with ExitStack() as cleanup:
        if radio:
            lease = cleanup.enter_context(service.socket("radio.sock"))
            lease.send(audio_wire.radio_play(999))
            assert lease.recv(32) == audio_wire.RADIO_READY
        assert media.command("WRITE 12000 10000") == "OK"
        previous = media.state()[0]
        service.change_jack(b"U")
        deadline = time.monotonic() + 3
        while True:
            with service.socket("status.sock") as diagnostic:
                state = audio_wire.status(diagnostic)
            if state["plugged"] == 0:
                break
            assert time.monotonic() < deadline
        assert (state["output"], state["applied"]) == (1, 1)
        if radio:
            assert state["fm_active"] == 1
            service.wait_state(media, state=1, reason=6)
            lease.settimeout(0.03)
            with pytest.raises(TimeoutError):
                lease.recv(32)
        else:
            service.wait_read(media, previous + 960)
            service.wait_state(media, state=0, reason=0)
        events = (service.directory / "events").read_text().splitlines()
        assert "PCM CLOSE" not in events
        assert "FM OFF" not in events

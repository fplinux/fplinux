# SPDX-License-Identifier: GPL-2.0-only
"""Production audio daemon with real sockets and rings, replacing only ALSA."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import mmap
import os
import socket
import struct
import subprocess
import sys
import time
from array import array
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-audiod"
FIXTURE = Path(__file__).parent / "fixtures/audiod"
MESSAGE = struct.Struct("=IIIIIIiI")
HELLO, OPEN, OPENED, ERROR, CLOSE, KICK = range(1, 7)
WRITE, READ, HEARD, STATE, SLEEPING = 16, 20, 24, 28, 32
PLAYING, PAUSED, UNAVAILABLE, ENDED = range(4)


def sample(frame: int) -> int:
    return 1000 + frame % 8000


class Ring:
    """Map an audiod-created memfd and act as its producer."""

    def __init__(self, descriptor: int) -> None:
        """Map the service's ring and close the transferred descriptor."""
        try:
            self.size = os.fstat(descriptor).st_size
            self.seals = fcntl.fcntl(descriptor, fcntl.F_GET_SEALS)
            self.mapping = mmap.mmap(descriptor, self.size)
        finally:
            os.close(descriptor)
        self.magic, self.rate, self.channels, self.capacity = struct.unpack_from(
            "=4I", self.mapping
        )

    def field(self, offset: int) -> int:
        """Read a published 32-bit ring counter or state."""
        value: int = struct.unpack_from("=I", self.mapping, offset)[0]
        return value

    def store(self, offset: int, value: int) -> None:
        """Write a mapped ring field, including deliberately corrupt test inputs."""
        struct.pack_into("=I", self.mapping, offset, value & 0xFFFFFFFF)

    def append(self, first: int, count: int, *, level: int | None = None) -> bool:
        """Queue frames and report whether the sleeping service needs a KICK."""
        write = self.field(WRITE)
        assert count <= self.capacity - ((write - self.field(READ)) & 0xFFFFFFFF)
        for frame in range(first, first + count):
            index = 64 + (write & (self.capacity - 1)) * self.channels * 2
            for channel in range(self.channels):
                value = sample(frame) if level is None else level
                struct.pack_into("=h", self.mapping, index + 2 * channel, value)
            write += 1
        self.store(WRITE, write)
        return self.field(SLEEPING) != 0


class StreamPeer:
    """Keep SO_PEERCRED tied to a live process without inheriting other clients."""

    def __init__(self, path: Path) -> None:
        """Start a live process that supplies each connection's peer credentials."""
        self.requests, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.requests.settimeout(3)
        try:
            self.process = subprocess.Popen(
                [sys.executable, "-I", str(FIXTURE / "stream_peer.py"), str(path)],
                stdin=child.fileno(),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
        finally:
            child.close()
        self.pid = self.process.pid

    def connect(self) -> socket.socket:
        """Request a new stream socket from the peer and take ownership."""
        self.requests.send(b"c")
        data, descriptors, _, _ = socket.recv_fds(self.requests, 1, 1)
        assert data == b"c"
        assert len(descriptors) == 1
        client = socket.socket(fileno=descriptors[0])
        client.settimeout(3)
        return client

    def close(self) -> None:
        """Close the request channel and reap the peer process."""
        self.requests.close()
        try:
            _, stderr = self.process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            _, stderr = self.process.communicate(timeout=3)
        assert self.process.returncode == 0, stderr


class AudioService:
    """Own one bounded daemon run, its real clients and its fake card files."""

    def __init__(self, executable: Path, work: Path) -> None:
        """Record the caller-owned directory for sockets and fake-card evidence."""
        self.executable = executable
        self.work = work
        self.events = work / "events"
        self.played = work / "played"
        self.busy = work / "busy"
        self.sockets: list[socket.socket] = []
        self.rings: list[Ring] = []
        self.peers: list[StreamPeer] = []
        self.process: subprocess.Popen[str] | None = None

    def run(
        self, check: Callable[[], None], **environment: str
    ) -> subprocess.CompletedProcess[str]:
        """Run the daemon for one exercise and always request shutdown."""

        def exercise(process: subprocess.Popen[str], _deadline: float) -> None:
            self.process = process
            try:
                self.wait((self.work / "stream.sock").exists, "daemon did not start")
                check()
            finally:
                process.terminate()

        return run_process(
            [str(self.executable), "--dir", str(self.work)],
            name="run production audio daemon with fake ALSA",
            timeout=15,
            env={
                "PATH": os.defpath,
                "FPLINUX_AUDIOD_FAKE_EVENTS": str(self.events),
                "FPLINUX_AUDIOD_FAKE_PLAYED": str(self.played),
                "FPLINUX_AUDIOD_FAKE_BUSY": str(self.busy),
                **environment,
            },
            while_running=exercise,
            check=True,
        )

    def close(self) -> None:
        """Release every client, mapping and peer even if a cleanup fails."""
        with contextlib.ExitStack() as cleanup:
            for peer in self.peers:
                cleanup.callback(peer.close)
            for ring in self.rings:
                cleanup.callback(ring.mapping.close)
            for client in self.sockets:
                cleanup.callback(client.close)

    def wait(self, condition: Callable[[], bool], message: str, timeout: float = 3) -> float:
        """Wait for an observed state and return its monotonic timestamp."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if condition():
                return time.monotonic()
            assert self.process is None or self.process.poll() is None, "daemon exited"
            time.sleep(0.005)
        raise AssertionError(message)

    def card_events(self) -> list[tuple[float, str]]:
        """Read timestamped operations recorded by the fake ALSA card."""
        if not self.events.exists():
            return []
        result = []
        for line in self.events.read_text(encoding="ascii").splitlines():
            stamp, event = line.split(" ", 1)
            result.append((float(stamp), event))
        return result

    def count(self, prefix: str) -> int:
        """Count recorded card operations with the requested prefix."""
        return sum(event.startswith(prefix) for _, event in self.card_events())

    def manager(self) -> socket.socket:
        """Connect a control socket owned by this fixture."""
        control = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        control.connect(str(self.work / "control.sock"))
        self.sockets.append(control)
        return control

    def peer(self) -> StreamPeer:
        """Start and retain a process for real stream peer credentials."""
        peer = StreamPeer(self.work / "stream.sock")
        self.peers.append(peer)
        return peer

    def connect(self, count: int = 1) -> tuple[list[socket.socket], int]:
        """Open stream sockets sharing one live peer PID."""
        peer = self.peer()
        clients = [peer.connect() for _ in range(count)]
        self.sockets.extend(clients)
        return clients, peer.pid

    @staticmethod
    def announce(
        control: socket.socket, pid: int, *, audible: bool = True, clock: bool = False
    ) -> None:
        """Declare the process's role and initial audibility."""
        command = (
            f"CLOCK {pid} {os.geteuid()}"
            if clock
            else (f"APP {pid} {os.geteuid()} org.fplinux.test")
        )
        control.send(command.encode())
        control.send(f"AUDIBLE {pid} {int(audible)}".encode())

    @staticmethod
    def send(client: socket.socket, kind: int, **fields: int) -> None:
        """Send one fixed-size stream request with unused fields zeroed."""
        client.send(
            MESSAGE.pack(
                kind,
                fields.get("version", 0),
                fields.get("stream", 0),
                fields.get("rate", 0),
                fields.get("channels", 0),
                fields.get("flags", 0),
                fields.get("error", 0),
                0,
            )
        )

    @staticmethod
    def receive(client: socket.socket) -> tuple[tuple[int, ...], list[int]]:
        """Receive a reply and transfer any attached descriptor to the caller."""
        data, descriptors, _, _ = socket.recv_fds(client, MESSAGE.size, 1)
        return (MESSAGE.unpack(data) if data else ()), descriptors

    def hello(self, client: socket.socket) -> None:
        """Send the supported stream handshake."""
        self.send(client, HELLO, version=1)

    def open_stream(
        self, client: socket.socket, rate: int = 48000, channels: int = 1
    ) -> tuple[int, Ring]:
        """Open a stream and retain its mapping for fixture cleanup."""
        self.send(client, OPEN, rate=rate, channels=channels)
        message, descriptors = self.receive(client)
        assert message[0] == OPENED, message
        assert len(descriptors) == 1
        ring = Ring(descriptors[0])
        self.rings.append(ring)
        return message[2], ring

    def refused(self, client: socket.socket, kind: int, **fields: int) -> int:
        """Send a rejected request and return its reported errno."""
        self.send(client, kind, **fields)
        message, descriptors = self.receive(client)
        assert descriptors == []
        assert message[0] == ERROR, message
        return message[6]

    @staticmethod
    def closed(client: socket.socket) -> bool:
        """Observe the peer's orderly close or connection reset."""
        try:
            return client.recv(MESSAGE.size) == b""
        except ConnectionResetError:
            return True

    def write(
        self, client: socket.socket, stream: int, ring: Ring, first: int, count: int
    ) -> None:
        """Queue sample frames and wake a sleeping stream."""
        if ring.append(first, count):
            self.send(client, KICK, stream=stream)

    def played_left(self) -> list[int]:
        """Read recorded mono output and verify identical stereo channels."""
        samples = array("h", self.played.read_bytes() if self.played.exists() else b"")
        assert samples[0::2] == samples[1::2], "mono must feed both channels"
        return list(samples[0::2])

    def playing_stream(self) -> tuple[socket.socket, socket.socket, int, int, Ring]:
        """Create one authorized, audible application stream."""
        control = self.manager()
        (client,), pid = self.connect()
        self.announce(control, pid)
        self.hello(client)
        stream, ring = self.open_stream(client)
        return control, client, pid, stream, ring


@pytest.fixture(scope="module")
def audiod_executable(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "audiod") as build:
        executable = build / "audiod"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Os",
                "-I",
                str(FIXTURE),
                "-I",
                str(ROOT / "include/fplinux"),
                *(
                    str(APORT / name)
                    for name in ("audiod.c", "audio-device.c", "audio-mixer.c", "audio-streams.c")
                ),
                str(ROOT / "lib/fplinux/fplinux-cli.c"),
                str(FIXTURE / "boundary.c"),
                "-pthread",
            ],
            name="compile production audio daemon with fake ALSA",
        )
        yield executable


@pytest.fixture
def audiod(audiod_executable: Path) -> Iterator[AudioService]:
    # Unix socket paths and the daemon's --dir must stay short.
    with TemporaryDirectory(prefix="fpa-") as scratch:
        service = AudioService(audiod_executable, Path(scratch))
        try:
            yield service
        finally:
            service.close()


def test_sockets_have_manager_and_application_permissions(audiod: AudioService) -> None:
    def exercise() -> None:
        assert (audiod.work / "control.sock").stat().st_mode & 0o777 == 0o600
        assert (audiod.work / "stream.sock").stat().st_mode & 0o777 == 0o660
        assert audiod.card_events() == []

    audiod.run(exercise)


def test_hello_rejects_unannounced_pid_wrong_uid_and_version(audiod: AudioService) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (stranger,), _ = audiod.connect()
        started = time.monotonic()
        audiod.hello(stranger)
        audiod.send(stranger, OPEN, rate=48000, channels=1)
        message, descriptors = audiod.receive(stranger)
        assert descriptors == []
        assert message[0] == ERROR
        assert message[6] == errno.EACCES
        assert 0.9 <= time.monotonic() - started < 1.8
        assert audiod.closed(stranger)

        (impostor,), pid = audiod.connect()
        control.send(f"APP {pid} {os.geteuid() + 1} org.fplinux.test".encode())
        assert audiod.refused(impostor, HELLO, version=1) == errno.EACCES
        assert audiod.closed(impostor)

        (old,), pid = audiod.connect()
        audiod.announce(control, pid)
        assert audiod.refused(old, HELLO, version=2) == errno.EPROTONOSUPPORT
        assert audiod.closed(old)

    audiod.run(exercise)


@pytest.mark.parametrize("clock", [False, True], ids=["application", "clock"])
def test_hello_waits_for_manager_announcement_and_starts_paused(
    audiod: AudioService, *, clock: bool
) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (client,), pid = audiod.connect()
        audiod.hello(client)
        audiod.send(client, OPEN, rate=22050, channels=2)
        time.sleep(0.1)
        command = (
            f"CLOCK {pid} {os.geteuid()}"
            if clock
            else (f"APP {pid} {os.geteuid()} org.fplinux.test")
        )
        control.send(command.encode())
        message, descriptors = audiod.receive(client)
        assert message[0] == OPENED
        assert message[2] != 0
        assert message[3:5] == (22050, 2)
        ring = Ring(descriptors[0])
        audiod.rings.append(ring)
        assert (ring.magic, ring.rate, ring.channels, ring.capacity) == (
            0x31415046,
            22050,
            2,
            8192,
        )
        assert ring.size == 64 + 8192 * 2 * 2
        sealed = fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_GROW | fcntl.F_SEAL_SEAL
        assert ring.seals & sealed == sealed
        assert (ring.field(STATE), ring.field(SLEEPING)) == (PAUSED, 1)

    audiod.run(exercise)


def test_pause_resume_preserves_every_frame_and_ramps_boundary(audiod: AudioService) -> None:
    stopped = 0

    def exercise() -> None:
        nonlocal stopped
        control, client, pid, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 9600)
        audiod.wait(lambda: ring.field(READ) >= 4800, "stream did not play")
        control.send(f"AUDIBLE {pid} 0".encode())
        barrier, _ = audiod.open_stream(client)
        audiod.send(client, CLOSE, stream=barrier)
        audiod.wait(lambda: ring.field(STATE) == PAUSED, "stream did not pause")
        stopped = ring.field(READ)
        time.sleep(0.1)
        assert ring.field(READ) == stopped < 9600
        control.send(f"AUDIBLE {pid} 1".encode())
        audiod.wait(
            lambda: ring.field(READ) == 9600 and ring.field(HEARD) == 9600,
            "resumed stream did not finish",
        )

    audiod.run(exercise)
    played = audiod.played_left()
    assert played[:stopped] == [sample(frame) for frame in range(stopped)]
    tail = played[stopped : stopped + 480]
    assert 0 < tail[0] < sample(stopped - 1)
    assert tail[-1] == 0
    assert tail == sorted(tail, reverse=True)
    resume = next(index for index in range(stopped + 480, len(played)) if played[index]) - 1
    for step in range(480):
        assert abs(played[resume + step] - sample(stopped + step) * step / 480) <= 2
    rest = 9600 - stopped - 480
    assert played[resume + 480 : resume + 480 + rest] == [
        sample(frame) for frame in range(stopped + 480, 9600)
    ]
    assert not any(played[resume + 480 + rest :])


def test_kick_opens_default_pcm_and_idle_releases_after_three_seconds(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control, client, pid, stream, ring = audiod.playing_stream()
        assert ring.field(SLEEPING) == 1
        assert ring.append(0, 2400)
        time.sleep(0.1)
        assert ring.field(READ) == 0
        assert audiod.count("open") == 0
        audiod.send(client, KICK, stream=stream)
        heard = audiod.wait(lambda: ring.field(HEARD) == 2400, "frames were not heard")
        closed = audiod.wait(lambda: audiod.count("close") == 1, "PCM stayed open", 5)
        assert 2.5 < closed - heard < 3.6
        assert (ring.field(STATE), ring.field(SLEEPING)) == (PLAYING, 1)
        assert [event for _, event in audiod.card_events() if event.startswith("open")] == [
            "open default"
        ]
        assert "hw 48000 2 480 1440" in [event for _, event in audiod.card_events()]
        control.send(f"AUDIBLE {pid} 0".encode())
        audiod.write(client, stream, ring, 2400, 2400)
        audiod.send(client, KICK, stream=stream)
        time.sleep(0.1)
        assert audiod.count("open") == 1
        assert ring.field(READ) == 2400

    result = audiod.run(exercise)
    assert "closed after 3000 ms of silence" in result.stdout


def test_busy_pcm_holds_positions_retries_once_per_second_and_recovers(
    audiod: AudioService,
) -> None:
    audiod.busy.touch()

    def exercise() -> None:
        _, client, _, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 4800)
        audiod.wait(lambda: audiod.count("busy") >= 2, "PCM was not retried", 2.5)
        assert (ring.field(STATE), ring.field(READ)) == (UNAVAILABLE, 0)
        busy_times = [stamp for stamp, event in audiod.card_events() if event == "busy"]
        assert 0.9 <= busy_times[1] - busy_times[0] < 1.5
        audiod.busy.unlink()
        audiod.wait(lambda: ring.field(READ) == 4800, "free PCM did not play", 2)
        assert ring.field(STATE) == PLAYING

    result = audiod.run(exercise)
    assert result.stdout.count("card default unavailable") == 1
    assert audiod.played_left()[:4800] == [sample(frame) for frame in range(4800)]


def test_pcm_underrun_retries_pending_period_without_losing_frames(audiod: AudioService) -> None:
    def exercise() -> None:
        _, client, _, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 4800)
        audiod.wait(
            lambda: ring.field(READ) == 4800 and ring.field(HEARD) == 4800,
            "stream did not finish after underrun",
        )
        assert audiod.count("xrun") == 1
        assert audiod.count("prepare") == 2

    result = audiod.run(exercise, FPLINUX_AUDIOD_FAKE_XRUN="5")
    assert result.stderr.count("underrun; prepared again") == 1
    assert audiod.played_left()[:4800] == [sample(frame) for frame in range(4800)]


def test_absent_pcm_holds_stream_without_repeated_warning(audiod: AudioService) -> None:
    def exercise() -> None:
        _, client, _, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 960)
        audiod.wait(lambda: audiod.count("absent") >= 2, "absent PCM was not retried", 2.5)
        assert (ring.field(STATE), ring.field(READ)) == (UNAVAILABLE, 0)

    result = audiod.run(exercise, FPLINUX_AUDIOD_FAKE_ABSENT="1")
    assert result.stdout.count("unavailable: No such file or directory") == 1


def test_gone_ends_all_process_connections_but_keeps_other_application(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (first, second), pid = audiod.connect(2)
        (other,), other_pid = audiod.connect()
        audiod.announce(control, pid)
        audiod.announce(control, other_pid)
        for client in (first, second, other):
            audiod.hello(client)
        _, ring_a = audiod.open_stream(first)
        _, ring_b = audiod.open_stream(second, 8000, 2)
        _, ring_c = audiod.open_stream(other)
        control.send(f"GONE {pid}".encode())
        assert audiod.closed(first)
        assert audiod.closed(second)
        assert (ring_a.field(STATE), ring_b.field(STATE)) == (ENDED, ENDED)
        _, ring_d = audiod.open_stream(other)
        assert (ring_c.field(STATE), ring_d.field(STATE)) == (PLAYING, PLAYING)

    audiod.run(exercise)


def test_gone_rejects_late_hello_until_same_live_process_is_announced(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control = audiod.manager()
        peer = audiod.peer()
        first = peer.connect()
        audiod.sockets.append(first)
        audiod.announce(control, peer.pid)
        audiod.hello(first)
        _, ring = audiod.open_stream(first)
        control.send(f"GONE {peer.pid}".encode())
        assert audiod.closed(first)
        assert ring.field(STATE) == ENDED
        late = peer.connect()
        audiod.sockets.append(late)
        started = time.monotonic()
        assert audiod.refused(late, HELLO, version=1) == errno.EAGAIN
        assert time.monotonic() - started < 0.5
        assert audiod.closed(late)
        audiod.announce(control, peer.pid)
        again = peer.connect()
        audiod.sockets.append(again)
        audiod.hello(again)
        _, ring = audiod.open_stream(again)
        assert ring.field(STATE) == PLAYING

    audiod.run(exercise)


def test_manager_drop_pauses_application_and_clock_resync_preserves_only_announced(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control = audiod.manager()
        clients = []
        for is_clock in (False, True, False):
            (client,), pid = audiod.connect()
            audiod.announce(control, pid, clock=is_clock)
            audiod.hello(client)
            stream, ring = audiod.open_stream(client)
            clients.append((client, pid, stream, ring))
        app, clock, lost = clients
        audiod.write(app[0], app[2], app[3], 0, 9600)
        audiod.write(clock[0], clock[2], clock[3], 0, 9600)
        audiod.wait(lambda: clock[3].field(READ) > 0, "clock did not play")
        control.close()
        audiod.wait(
            lambda: all(ring.field(STATE) == PAUSED for _, _, _, ring in clients),
            "manager loss did not pause all rings",
        )
        app_read, clock_read = app[3].field(READ), clock[3].field(READ)
        time.sleep(0.1)
        assert (app[3].field(READ), clock[3].field(READ)) == (app_read, clock_read)
        control = audiod.manager()
        connected = time.monotonic()
        audiod.announce(control, app[1])
        audiod.announce(control, clock[1], clock=True)
        audiod.wait(
            lambda: app[3].field(READ) == 9600 and clock[3].field(READ) == 9600,
            "resynced streams did not resume",
        )
        ended = audiod.wait(lambda: lost[3].field(STATE) == ENDED, "unannounced ring survived")
        assert 0.8 < ended - connected < 1.6
        assert audiod.closed(lost[0])
        assert app[3].field(STATE) == PLAYING
        assert clock[3].field(STATE) == PLAYING

    audiod.run(exercise)


def test_client_hangup_ends_rings_and_releases_process_stream_limit(audiod: AudioService) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (first, second), pid = audiod.connect(2)
        audiod.announce(control, pid)
        audiod.hello(first)
        audiod.hello(second)
        rings = [audiod.open_stream(first)[1] for _ in range(4)]
        assert audiod.refused(second, OPEN, rate=48000, channels=1) == errno.EMFILE
        first.close()
        audiod.wait(lambda: all(ring.field(STATE) == ENDED for ring in rings), "HUP kept rings")
        _, ring = audiod.open_stream(second)
        assert ring.field(STATE) == PLAYING

    audiod.run(exercise)


def test_corrupt_ring_ends_only_itself_and_ignores_public_header_changes(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        _, client, _, stream, ring = audiod.playing_stream()
        other_stream, other = audiod.open_stream(client)
        audiod.write(client, stream, ring, 0, 4800)
        audiod.write(client, other_stream, other, 0, 4800)
        # Ring metadata is writable; the consumer must keep its own bounds.
        ring.store(8, 0xFFFFFFFF)
        ring.store(12, 0xFFFFFFFF)
        audiod.wait(lambda: ring.field(READ) > 0, "stream did not play")
        ring.store(WRITE, ring.field(READ) + ring.capacity + 1)
        audiod.wait(lambda: ring.field(STATE) == ENDED, "corrupt counter was consumed")
        audiod.wait(lambda: other.field(READ) == 4800, "other stream stopped")
        _, replacement = audiod.open_stream(client)
        assert replacement.field(STATE) == PLAYING

    audiod.run(exercise)


def test_clock_reserved_ring_survives_full_app_pool_and_app_pause_then_gone(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control = audiod.manager()
        applications = []
        for _ in range(3):
            (client,), pid = audiod.connect()
            audiod.announce(control, pid)
            audiod.hello(client)
            applications.append((client, pid))
        first, second, third = applications
        app_stream, app_ring = audiod.open_stream(first[0])
        for _ in range(3):
            audiod.open_stream(first[0])
        for _ in range(4):
            audiod.open_stream(second[0])
        assert audiod.refused(first[0], OPEN, rate=48000, channels=1) == errno.EMFILE
        # Same uid, a clock-looking app ID and a client flag grant no role.
        control.send(f"APP {third[1]} {os.geteuid()} org.fplinux.clock".encode())
        control.send(f"AUDIBLE {third[1]} 1".encode())
        assert audiod.refused(third[0], OPEN, rate=48000, channels=1, flags=1) == errno.EINVAL
        assert audiod.refused(third[0], OPEN, rate=48000, channels=1) == errno.EBUSY
        (clock,), clock_pid = audiod.connect()
        audiod.announce(control, clock_pid, clock=True, audible=False)
        audiod.hello(clock)
        clock_stream, clock_ring = audiod.open_stream(clock)
        assert clock_ring.field(STATE) == PAUSED
        assert audiod.refused(clock, OPEN, rate=48000, channels=1) == errno.EMFILE
        audiod.write(first[0], app_stream, app_ring, 0, 9600)
        audiod.wait(lambda: app_ring.field(READ) >= 960, "app stream did not play")
        for _, pid in applications:
            control.send(f"AUDIBLE {pid} 0".encode())
        audiod.wait(lambda: app_ring.field(STATE) == PAUSED, "app did not pause")
        stopped = app_ring.field(READ)
        control.send(f"AUDIBLE {clock_pid} 1".encode())
        clock_ring.append(0, 9600, level=12345)
        audiod.send(clock, KICK, stream=clock_stream)
        audiod.wait(lambda: clock_ring.field(READ) >= 960, "clock reserved ring did not play")
        assert app_ring.field(READ) == stopped
        assert 12345 in audiod.played_left(), "the clock signal did not reach PCM output"
        control.send(f"GONE {clock_pid}".encode())
        assert audiod.closed(clock)
        assert clock_ring.field(STATE) == ENDED
        clock_stopped = clock_ring.field(READ)
        clock_ring.append(9600, 480)
        control.send(f"AUDIBLE {first[1]} 1".encode())
        audiod.wait(lambda: app_ring.field(READ) == 9600, "CLOCK GONE stopped application")
        assert clock_ring.field(READ) == clock_stopped
        assert app_ring.field(STATE) == PLAYING

    audiod.run(exercise)


def test_resync_cannot_change_existing_application_connection_into_clock(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control, client, pid, _, ring = audiod.playing_stream()
        control.close()
        audiod.wait(lambda: ring.field(STATE) == PAUSED, "manager loss did not pause stream")
        control = audiod.manager()
        audiod.announce(control, pid, clock=True)
        assert audiod.closed(client)
        assert ring.field(STATE) == ENDED

    audiod.run(exercise)


def test_clock_hello_rejects_wrong_uid_and_second_clock_process(audiod: AudioService) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (wrong,), wrong_pid = audiod.connect()
        control.send(f"CLOCK {wrong_pid} {os.geteuid() + 1}".encode())
        assert audiod.refused(wrong, HELLO, version=1) == errno.EACCES
        assert audiod.closed(wrong)
        control.send(f"GONE {wrong_pid}".encode())
        (clock,), pid = audiod.connect()
        audiod.announce(control, pid, clock=True)
        audiod.hello(clock)
        _, ring = audiod.open_stream(clock)
        (second,), second_pid = audiod.connect()
        audiod.announce(control, second_pid, clock=True)
        assert audiod.refused(second, HELLO, version=1) == errno.EACCES
        assert audiod.closed(second)
        assert ring.field(STATE) == PLAYING

    audiod.run(exercise)


@pytest.mark.parametrize(
    ("rate", "channels", "flags"),
    [(7999, 1, 0), (48001, 1, 0), (8000, 0, 0), (8000, 3, 0), (8000, 1, 1)],
)
def test_open_rejects_unsupported_format_without_losing_connection(
    audiod: AudioService, rate: int, channels: int, flags: int
) -> None:
    def exercise() -> None:
        _, client, _, _, _ = audiod.playing_stream()
        assert (
            audiod.refused(client, OPEN, rate=rate, channels=channels, flags=flags) == errno.EINVAL
        )
        audiod.open_stream(client)

    audiod.run(exercise)


def test_malformed_stream_packet_hangs_up_only_its_connection(audiod: AudioService) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (client, other), pid = audiod.connect(2)
        audiod.announce(control, pid)
        audiod.hello(client)
        audiod.hello(other)
        _, ring = audiod.open_stream(client)
        _, other_ring = audiod.open_stream(other)
        client.send(b"\0" * (MESSAGE.size - 1))
        assert audiod.closed(client)
        assert ring.field(STATE) == ENDED
        assert other_ring.field(STATE) == PLAYING
        audiod.open_stream(other)

    audiod.run(exercise)

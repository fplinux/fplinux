# SPDX-License-Identifier: GPL-2.0-only
"""Production audio daemon with real sockets and rings, replacing only ALSA."""

from __future__ import annotations

import contextlib
import errno
import fcntl
import mmap
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import termios
import time
from array import array
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import TYPE_CHECKING

import pytest

from tests import audio_wire
from tests.c_harness import c_build_directory, compile_c
from tests.process import connect_unix_seqpacket, run_process

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator

ROOT = Path(__file__).resolve().parents[3]
APORT = ROOT / "alpine/aports/fplinux-audiod"
FIXTURE = Path(__file__).parent / "fixtures/audiod"
MESSAGE = struct.Struct("=IIIIIIiI")
HELLO, OPEN, OPENED, ERROR, CLOSE, KICK = range(1, 7)
PAUSE = 7
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

    def __init__(self, path: Path, cleanup: contextlib.ExitStack) -> None:
        """Start a live process that supplies each connection's peer credentials."""
        self.requests, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        with child, contextlib.ExitStack() as failed_start:
            failed_start.callback(self.requests.close)
            self.requests.settimeout(3)
            self.process = subprocess.Popen(
                [sys.executable, "-I", str(FIXTURE / "stream_peer.py"), str(path)],
                stdin=child.fileno(),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                start_new_session=True,
            )
            cleanup.callback(self.close)
            failed_start.pop_all()
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

    def __init__(self, executable: Path, work: Path, cleanup: contextlib.ExitStack) -> None:
        """Record the caller-owned directory for sockets and fake-card evidence."""
        self.executable = executable
        self.work = work
        self.cleanup = cleanup
        self.events = work / "events"
        self.played = work / "played"
        self.busy = work / "busy"
        self.radio_ready = work / "radio-ready"
        self.sockets: list[socket.socket] = []
        self.rings: list[Ring] = []
        self.process: subprocess.Popen[str] | None = None
        self.activities: list[tuple[int, int, str]] = []

    def run(
        self, check: Callable[[], None], **environment: str
    ) -> subprocess.CompletedProcess[str]:
        """Run the daemon for one exercise and always request shutdown."""

        def exercise(process: subprocess.Popen[str], _deadline: float) -> None:
            self.process = process
            try:
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
                "FPLINUX_AUDIOD_FAKE_RADIO_READY": str(self.radio_ready),
                **environment,
            },
            while_running=exercise,
            check=True,
        )

    def close(self) -> None:
        """Release every client and mapping even if a cleanup fails."""
        with contextlib.ExitStack() as cleanup:
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

    def manager(self, *, sync: bool = True) -> socket.socket:
        """Connect a control socket owned by this fixture."""
        control = connect_unix_seqpacket(self.work / "control.sock")
        self.sockets.append(control)
        for kind in ("MEDIA", "ALARM"):
            self.set_volume(control, kind, 10)
        if sync:
            control.send(audio_wire.SYNC)
        return control

    def status(self) -> dict[str, int]:
        """Request an actual read-only device snapshot from this daemon."""
        with connect_unix_seqpacket(self.work / "status.sock") as status:
            return audio_wire.status(status)

    def radio(self) -> socket.socket:
        """Acquire one real service FM lease against the tuner boundary."""
        radio = connect_unix_seqpacket(self.work / "radio.sock")
        self.sockets.append(radio)
        radio.send(audio_wire.radio_play(999))
        assert radio.recv(32) == audio_wire.RADIO_READY
        return radio

    def activity(self, packet: bytes) -> tuple[int, int, str]:
        """Validate and retain one asynchronous manager activity notification."""
        result = audio_wire.activity(packet)
        self.activities.append(result)
        return result

    def expect_control(self, control: socket.socket, expected: bytes) -> None:
        """Keep asynchronous activity separate from an exact command reply."""
        while True:
            packet = control.recv(256)
            if audio_wire.is_activity(packet):
                self.activity(packet)
            else:
                assert packet == expected
                return

    def set_volume(self, control: socket.socket, kind: str, level: int) -> None:
        """Require the service to acknowledge the applied class and level."""
        control.send(audio_wire.set_volume(kind, level))
        self.expect_control(control, audio_wire.volume_applied(kind, level))

    def expect_activity(self, control: socket.socket, pid: int, state: str) -> None:
        """Wait for a service-owned state of this exact application process."""
        while self.activity(control.recv(256)) != (pid, os.geteuid(), state):
            pass

    def peer(self) -> StreamPeer:
        """Start and retain a process for real stream peer credentials."""
        return StreamPeer(self.work / "stream.sock", self.cleanup)

    def connect(self, count: int = 1) -> tuple[list[socket.socket], int]:
        """Open stream sockets sharing one live peer PID."""
        peer = self.peer()
        clients = []
        for _ in range(count):
            client = peer.connect()
            self.sockets.append(client)
            clients.append(client)
        return clients, peer.pid

    @staticmethod
    def announce(
        control: socket.socket,
        pid: int,
        *,
        eligible: bool = True,
        clock: bool = False,
        background: bool = False,
    ) -> None:
        """Declare the process's trusted role and foreground eligibility."""
        command = (
            audio_wire.clock(pid, os.geteuid())
            if clock
            else audio_wire.app(pid, os.geteuid(), background=background)
        )
        control.send(command)
        control.send(audio_wire.eligible(pid, enabled=eligible))

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
                str(ROOT / "lib/fplinux-shared/fplinux-audio-control.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-audio-radio.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-audio-status.c"),
                str(ROOT / "lib/fplinux-shared/fplinux-cli.c"),
                str(FIXTURE / "boundary.c"),
                "-pthread",
            ],
            name="compile production audio daemon with fake ALSA",
        )
        yield executable


@pytest.fixture
def audiod(audiod_executable: Path) -> Iterator[AudioService]:
    # Unix socket paths and the daemon's --dir must stay short.
    with TemporaryDirectory(prefix="fpa-") as scratch, contextlib.ExitStack() as cleanup:
        service = AudioService(audiod_executable, Path(scratch), cleanup)
        try:
            yield service
        finally:
            service.close()


def test_session_client_replays_after_queue_pressure_and_output_refusal(
    audiod: AudioService, tmp_path: Path
) -> None:
    client = tmp_path / "session-audio"
    session = ROOT / "alpine/aports/fplinux-session"
    compile_c(
        client,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            f"-I{session}",
            str(ROOT / "tests/host_tool/gui/session/session-audio-test.c"),
            str(session / "session-audio.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-audio-control.c"),
        ],
        name="compile the production Session client for audiod",
    )

    def exercise() -> None:
        assert audiod.process is not None
        process = audiod.process
        control = audiod.work / "control.sock"
        audiod.wait(control.exists, "audiod did not open the control socket")
        try:
            result = run_process(
                [str(client), "--live", str(control), str(process.pid)],
                name="run the production Session client through audiod control",
                timeout=11,
                check=True,
            )
        finally:
            # The harness pauses only our daemon while filling the socket queue.
            with contextlib.suppress(ProcessLookupError):
                process.send_signal(signal.SIGCONT)
        assert result.stdout == "session audio: OK\n"
        state = audiod.status()
        assert (state["media"], state["alarm"], state["output"], state["applied"]) == (4, 0, 0, 2)

    audiod.run(exercise, FPLINUX_AUDIOD_FAKE_NO_PROFILE="1")


def test_activity_tracks_real_queued_playback_pause_focus_and_close(audiod: AudioService) -> None:
    hold_pcm = audiod.work / "hold-pcm"

    def exercise() -> None:
        control = audiod.manager(sync=False)
        (media,), pid = audiod.connect()
        audiod.announce(control, pid, eligible=False, background=True)
        audiod.hello(media)
        stream, ring = audiod.open_stream(media)
        assert not select.select([control], [], [], 0.05)[0]
        control.send(audio_wire.SYNC)
        audiod.expect_activity(control, pid, "none")
        assert not select.select([control], [], [], 0.05)[0]
        audiod.write(media, stream, ring, 0, 9600)
        audiod.expect_activity(control, pid, "playing")
        audiod.send(media, PAUSE, stream=stream, flags=1)
        audiod.expect_activity(control, pid, "paused")
        hold_pcm.touch()
        audiod.wait(lambda: audiod.count("pcm hold") == 1, "PCM did not acknowledge the hold")
        media_read = ring.field(READ)
        audiod.write(media, stream, ring, 9600, 4800)
        (clock,), clock_pid = audiod.connect()
        audiod.announce(control, clock_pid, clock=True)
        audiod.hello(clock)
        clock_stream, clock_ring = audiod.open_stream(clock)
        audiod.write(clock, clock_stream, clock_ring, 0, 9600)
        audiod.wait(lambda: audiod.status()["clock"] == 1, "Clock did not take focus")
        assert ring.field(WRITE) == 14400
        assert ring.field(READ) == media_read < 14400
        assert (clock_ring.field(WRITE), clock_ring.field(READ)) == (9600, 0)

        audiod.send(media, PAUSE, stream=stream, flags=0)
        assert audiod.activity(control.recv(256)) == (pid, os.geteuid(), "held")
        assert (ring.field(STATE), ring.field(READ)) == (PAUSED, media_read)
        assert (clock_ring.field(READ), clock_ring.field(HEARD)) == (0, 0)
        hold_pcm.unlink()
        assert audiod.activity(control.recv(256)) == (pid, os.geteuid(), "playing")
        assert clock_ring.field(HEARD) == 9600
        audiod.expect_activity(control, pid, "none")
        assert ring.field(HEARD) == 14400

        # Keep the new batch queued so only CLOSE can clear its activity.
        hold_pcm.touch()
        audiod.wait(lambda: audiod.count("pcm hold") == 2, "PCM did not acknowledge the hold")
        audiod.write(media, stream, ring, 14400, 480)
        audiod.expect_activity(control, pid, "playing")
        assert (ring.field(READ), ring.field(WRITE)) == (14400, 14880)
        audiod.send(media, CLOSE, stream=stream)
        audiod.expect_activity(control, pid, "none")
        assert ring.field(STATE) == ENDED
        assert all(owner != clock_pid for owner, _, _ in audiod.activities)

    audiod.run(exercise, FPLINUX_AUDIOD_FAKE_HOLD_PCM=str(hold_pcm))


def test_activity_reconnect_waits_for_sync_and_reports_current_output_state(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        control = audiod.manager()
        (media,), pid = audiod.connect()
        audiod.announce(control, pid, background=True)
        audiod.expect_activity(control, pid, "none")
        audiod.hello(media)
        stream, ring = audiod.open_stream(media)
        audiod.busy.touch()
        audiod.write(media, stream, ring, 0, 9600)
        audiod.expect_activity(control, pid, "unavailable")
        control.close()
        audiod.wait(lambda: ring.field(STATE) == PAUSED, "manager loss did not pause")
        control = audiod.manager(sync=False)
        audiod.announce(control, pid, background=True)
        assert not select.select([control], [], [], 0.05)[0]
        control.send(audio_wire.SYNC)
        audiod.expect_activity(control, pid, "unavailable")
        control.send(audio_wire.gone(pid))
        audiod.wait(lambda: ring.field(STATE) == ENDED, "GONE kept the stream")
        assert not select.select([control], [], [], 0.05)[0]

    audiod.run(exercise)


def test_sockets_have_manager_and_application_permissions(audiod: AudioService) -> None:
    def exercise() -> None:
        for name, mode in (
            ("control.sock", 0o600),
            ("stream.sock", 0o660),
            ("status.sock", 0o660),
            ("radio.sock", 0o600),
        ):
            path = audiod.work / name
            with connect_unix_seqpacket(path):
                assert path.stat().st_mode & 0o777 == mode
        assert audiod.count("open ") == 0

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
        control.send(audio_wire.app(pid, os.geteuid() + 1))
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
        # The sender queue empties when the daemon consumes the HELLO packet.
        audiod.wait(
            lambda: fcntl.ioctl(client, termios.TIOCOUTQ, bytes(4)) == bytes(4),
            "daemon did not consume HELLO",
        )
        audiod.send(client, OPEN, rate=22050, channels=2)
        readable, _, _ = select.select([client], [], [], 0.1)
        assert not readable, "stream replied before the manager identified its peer"
        command = (
            audio_wire.clock(pid, os.geteuid()) if clock else audio_wire.app(pid, os.geteuid())
        )
        control.send(command)
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
        control.send(audio_wire.eligible(pid, enabled=False))
        barrier, _ = audiod.open_stream(client)
        audiod.send(client, CLOSE, stream=barrier)
        audiod.wait(lambda: ring.field(STATE) == PAUSED, "stream did not pause")
        stopped = ring.field(READ)
        time.sleep(0.1)
        assert ring.field(READ) == stopped < 9600
        control.send(audio_wire.eligible(pid, enabled=True))
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


def test_class_volume_ack_mute_and_reconnect_for_app_and_clock(audiod: AudioService) -> None:
    def exercise() -> None:
        control = audiod.manager()
        control.settimeout(3)
        for kind in ("MEDIA", "ALARM"):
            audiod.set_volume(control, kind, 0)
        owners = []
        for clock in (False, True):
            (client,), pid = audiod.connect()
            audiod.announce(control, pid, clock=clock)
            audiod.hello(client)
            stream, ring = audiod.open_stream(client)
            owners.append((client, pid, stream, ring))
            ring.append(0, 4800, level=20000)
            audiod.send(client, KICK, stream=stream)
        audiod.wait(
            lambda: all(ring.field(READ) == 4800 for _, _, _, ring in owners),
            "mute did not consume both streams",
        )
        assert not any(audiod.played_left())
        control.close()
        audiod.wait(
            lambda: all(ring.field(STATE) == PAUSED for _, _, _, ring in owners),
            "manager loss did not pause streams",
        )
        control = audiod.manager(sync=False)
        control.settimeout(3)
        audiod.set_volume(control, "MEDIA", 5)
        audiod.set_volume(control, "ALARM", 10)
        for index, (client, pid, stream, ring) in enumerate(owners):
            ring.append(4800, 4800, level=20000)
            audiod.announce(control, pid, clock=bool(index))
            audiod.send(client, KICK, stream=stream)
        control.send(audio_wire.SYNC)
        audiod.wait(
            lambda: all(ring.field(READ) == 9600 for _, _, _, ring in owners),
            "resynced streams did not play",
        )

    audiod.run(exercise)
    played = audiod.played_left()
    assert max(played) == 20000
    assert 3556 in played


def test_invalid_volume_packets_leave_both_levels_unchanged(audiod: AudioService) -> None:
    def exercise() -> None:
        control = audiod.manager()
        audiod.set_volume(control, "MEDIA", 0)
        for command in (
            b"\x01\x01",
            b"\x01\x01\x07\x00",
            b"\x01\x01\x07" + bytes(32),
            b"\x01\x03\x07",
            b"\x01\x02\x0b",
            b"\x08\x01\x07",
        ):
            control.send(command)
        control.send(audio_wire.set_output(0))
        audiod.expect_control(control, audio_wire.output_applied(0))
        status = audiod.status()
        assert (status["media"], status["alarm"]) == (0, 10)

    audiod.run(exercise)


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
        audiod.wait(lambda: ring.field(HEARD) == 2400, "frames were not heard")
        audiod.wait(lambda: audiod.count("close") == 1, "PCM stayed open", 5)
        events = audiod.card_events()
        signals = [stamp for stamp, event in events if event == "pcm signal"]
        (closed,) = [stamp for stamp, event in events if event == "close"]
        assert signals
        assert 2.5 < closed - signals[-1] < 3.6
        assert (ring.field(STATE), ring.field(SLEEPING)) == (PLAYING, 1)
        assert [event for _, event in events if event.startswith("open")] == ["open default"]
        assert "hw 48000 2 480 1440" in [event for _, event in events]
        control.send(audio_wire.eligible(pid, enabled=False))
        audiod.wait(lambda: ring.field(STATE) == PAUSED, "stream did not pause")
        audiod.write(client, stream, ring, 2400, 2400)
        audiod.send(client, KICK, stream=stream)
        time.sleep(0.1)
        assert audiod.count("open") == 1
        assert ring.field(READ) == 2400

    result = audiod.run(exercise, FPLINUX_AUDIOD_FAKE_SIGNAL_EVENTS="1")
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
        control.send(audio_wire.gone(pid))
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
        control.send(audio_wire.gone(peer.pid))
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
        control = audiod.manager(sync=False)
        connected = time.monotonic()
        audiod.announce(control, app[1])
        audiod.announce(control, clock[1], clock=True)
        control.send(audio_wire.SYNC)
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
        control.send(audio_wire.app(third[1], os.geteuid()))
        control.send(audio_wire.eligible(third[1], enabled=True))
        assert audiod.refused(third[0], OPEN, rate=48000, channels=1, flags=1) == errno.EINVAL
        assert audiod.refused(third[0], OPEN, rate=48000, channels=1) == errno.EBUSY
        (clock,), clock_pid = audiod.connect()
        audiod.announce(control, clock_pid, clock=True, eligible=False)
        audiod.hello(clock)
        clock_stream, clock_ring = audiod.open_stream(clock)
        assert clock_ring.field(STATE) == PAUSED
        assert audiod.refused(clock, OPEN, rate=48000, channels=1) == errno.EMFILE
        audiod.write(first[0], app_stream, app_ring, 0, 9600)
        audiod.wait(lambda: app_ring.field(READ) >= 960, "app stream did not play")
        for _, pid in applications:
            control.send(audio_wire.eligible(pid, enabled=False))
        audiod.wait(lambda: app_ring.field(STATE) == PAUSED, "app did not pause")
        stopped = app_ring.field(READ)
        control.send(audio_wire.eligible(clock_pid, enabled=True))
        clock_ring.append(0, 9600, level=12345)
        audiod.send(clock, KICK, stream=clock_stream)
        audiod.wait(lambda: clock_ring.field(READ) >= 960, "clock reserved ring did not play")
        assert app_ring.field(READ) == stopped
        assert 12345 in audiod.played_left(), "the clock signal did not reach PCM output"
        control.send(audio_wire.gone(clock_pid))
        assert audiod.closed(clock)
        assert clock_ring.field(STATE) == ENDED
        clock_stopped = clock_ring.field(READ)
        clock_ring.append(9600, 480)
        control.send(audio_wire.eligible(first[1], enabled=True))
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
        control.send(audio_wire.clock(wrong_pid, os.geteuid() + 1))
        assert audiod.refused(wrong, HELLO, version=1) == errno.EACCES
        assert audiod.closed(wrong)
        control.send(audio_wire.gone(wrong_pid))
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


def test_late_fm_node_changes_status_without_restarting_daemon(audiod: AudioService) -> None:
    def exercise() -> None:
        assert audiod.process is not None
        pid = audiod.process.pid
        assert audiod.status()["fm"] == 0
        audiod.radio_ready.touch()
        audiod.wait(
            lambda: audiod.status()["fm"] == 1,
            "New FM node did not become available without restarting",
        )
        assert audiod.process.pid == pid
        assert audiod.process.poll() is None
        control = audiod.manager()
        before = audiod.card_events()
        audiod.set_volume(control, "MEDIA", 6)
        assert audiod.card_events() == before

    audiod.run(exercise)


def test_fm_rejects_invalid_packets_and_reports_device_error_before_releasing(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        path = audiod.work / "radio.sock"
        for packet in (
            b"\x02\xe7",
            b"\x02\xe7\x03\x00",
            b"\x02\xe7\x03" + bytes(32),
            b"\x02\x6a\x03",
            b"\x03",
        ):
            with connect_unix_seqpacket(path) as radio:
                radio.send(packet)
                assert radio.recv(32) == b""
        with connect_unix_seqpacket(path) as radio:
            radio.send(b"\x02\xe7\x03")
            assert radio.recv(32) == b"\x04\x02\x00\x00\x00"
            assert radio.recv(32) == b""
        with connect_unix_seqpacket(path) as scan:
            scan.send(b"\x01")
            assert scan.recv(32) == b"\x03"
            with connect_unix_seqpacket(path) as competing:
                assert competing.recv(32) == b""

    audiod.run(exercise)


def test_fm_frontend_reports_refusal_and_releases_playback_lease(
    audiod: AudioService, tmp_path: Path
) -> None:
    frontend = tmp_path / "fplinux-fm"
    compile_c(
        frontend,
        [
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include/fplinux'}",
            str(ROOT / "alpine/aports/fplinux-fm/fplinux-fm.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-audio-radio.c"),
            str(ROOT / "lib/fplinux-shared/fplinux-cli.c"),
            str(FIXTURE / "fm-socket-path.c"),
            "-Wl,--wrap=connect",
        ],
        name="compile FM frontend with the audiod fixture socket path",
    )

    def exercise() -> None:
        socket_path = audiod.work / "radio.sock"
        audiod.wait(socket_path.exists, "audiod did not open the FM socket")

        def play() -> subprocess.CompletedProcess[str]:
            return run_process(
                [str(frontend), "play", "98.3", "--seconds=1"],
                name="request FM playback from the real audiod",
                timeout=4,
                env={"FPLINUX_TEST_RADIO_SOCKET": str(socket_path)},
            )

        refused = play()
        assert refused.returncode == 1
        assert refused.stdout == ""
        assert refused.stderr == f"fplinux-fm: audio service: {os.strerror(errno.ENOENT)}\n"
        audiod.radio_ready.touch()
        playing = play()
        assert playing.returncode == 0, playing.stderr
        assert playing.stdout == "Playing 98.3 MHz; press Ctrl-C to stop.\n"
        audiod.wait(lambda: audiod.status()["fm_active"] == 0, "FM lease did not close")
        assert audiod.count("radio tune 983") == 1

    audiod.run(exercise)


def test_pcm_maximum_reference_and_fm_volume_survive_routes_alarm_and_cleanup(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        audiod.radio_ready.touch()
        control, client, _, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 4800)
        audiod.wait(lambda: ring.field(HEARD) == 4800, "Initial PCM did not drain")
        assert "pcm levels 9 9 9" in [event for _, event in audiod.card_events()]
        assert audiod.played_left()[:4800] == [sample(frame) for frame in range(4800)]
        audiod.set_volume(control, "MEDIA", 7)
        radio = audiod.radio()
        assert "radio levels 6 9 9" in [event for _, event in audiod.card_events()]
        control.send(audio_wire.set_output(2))
        audiod.expect_control(control, audio_wire.output_applied(2))
        assert audiod.status()["applied"] == 2
        assert audiod.card_events()[-1][1] == "radio levels 6 6 6"
        before_mute = len(audiod.card_events())
        audiod.set_volume(control, "MEDIA", 0)
        muted_events = [event for _, event in audiod.card_events()[before_mute:]]
        assert "radio mute 0" not in muted_events
        assert audiod.status()["media"] == 0
        control.send(audio_wire.set_output(1))
        audiod.expect_control(control, audio_wire.output_applied(1))
        assert audiod.status()["applied"] == 1
        assert "speaker volume 0" in [event for _, event in audiod.card_events()]
        (clock,), clock_pid = audiod.connect()
        audiod.announce(control, clock_pid, clock=True)
        audiod.hello(clock)
        clock_stream, clock_ring = audiod.open_stream(clock)
        clock_ring.append(0, 4800, level=20000)
        audiod.send(clock, KICK, stream=clock_stream)
        audiod.wait(lambda: clock_ring.field(HEARD) == 4800, "Alarm did not drain during muted FM")
        audiod.wait(
            lambda: audiod.status()["fm_active"] == 1,
            "FM did not resume after Alarm",
        )
        assert 20000 in audiod.played_left()
        assert "pcm levels 9 9 9" in [event for _, event in audiod.card_events()]
        assert audiod.card_events()[-1][1] == "fm on"
        audiod.set_volume(control, "MEDIA", 7)
        assert audiod.card_events()[-1][1] == "radio levels 6 9 9"
        radio.close()
        audiod.wait(lambda: audiod.status()["fm_active"] == 0, "FM lease did not close")
        first_media = len(audiod.played_left())
        ring.append(4800, 4800, level=20000)
        audiod.send(client, KICK, stream=stream)
        audiod.wait(lambda: ring.field(HEARD) == 9600, "PCM did not resume after FM")
        assert max(audiod.played_left()[first_media:]) == 7096
        events = [event for _, event in audiod.card_events()]
        assert {event for event in events if event.startswith("pcm levels ")} == {
            "pcm levels 9 9 9"
        }

    levels = audiod.work / "levels"
    audiod.run(
        exercise,
        FPLINUX_AUDIOD_FAKE_LEVELS=str(levels),
    )
    assert levels.read_text(encoding="ascii") == "6 3 7\n"


@pytest.mark.parametrize(
    ("headphones_only", "ordinals"),
    [
        (False, [1, 1, 2, 3, 4, 5, 6, 7, 8, 9]),
        (True, [1, 1, 2, 2, 3, 3, 4, 4, 5, 6]),
    ],
)
def test_exclusive_fm_media_levels_use_available_hardware_range_and_zero_mutes(
    audiod: AudioService, *, headphones_only: bool, ordinals: list[int]
) -> None:
    def exercise() -> None:
        audiod.radio_ready.touch()
        control = audiod.manager()
        audiod.radio()
        for level, ordinal in enumerate(ordinals, start=1):
            audiod.set_volume(control, "MEDIA", level)
            expected = (
                f"radio levels 6 {ordinal} {ordinal}"
                if headphones_only
                else f"radio levels {ordinal} 9 9"
            )
            assert audiod.card_events()[-1][1] == expected
            assert audiod.status()["media"] == level
        before_mute = len(audiod.card_events())
        audiod.set_volume(control, "MEDIA", 0)
        events = [event for _, event in audiod.card_events()[before_mute:]]
        assert "radio mute 1" in events
        assert "radio mute 0" not in events
        assert (audiod.status()["fm_active"], audiod.status()["media"]) == (1, 0)
        assert audiod.count("open ") == 0

    audiod.run(exercise, **({"FPLINUX_AUDIOD_FAKE_NO_PROFILE": "1"} if headphones_only else {}))


def test_failed_fm_gain_does_not_acknowledge_or_commit_requested_media_level(
    audiod: AudioService,
) -> None:
    failure = audiod.work / "fail-gain"

    def exercise() -> None:
        audiod.radio_ready.touch()
        control = audiod.manager()
        audiod.set_volume(control, "MEDIA", 7)
        audiod.radio()
        failure.write_text("4\n", encoding="ascii")
        control.send(audio_wire.set_volume("MEDIA", 5))
        assert audiod.closed(control)
        assert audiod.status()["media"] == 7
        assert audiod.status()["error"] == errno.EIO
        failure.unlink()
        control = audiod.manager()
        audiod.set_volume(control, "MEDIA", 7)
        assert audiod.status()["media"] == 7
        assert audiod.card_events()[-1][1] == "radio levels 6 9 9"

    audiod.run(exercise, FPLINUX_AUDIOD_FAKE_FAIL_GAIN=str(failure))


@pytest.mark.parametrize(
    "failure",
    ["info", "type", "count", "minimum", "maximum", "unreadable", "readonly", "read", "write"],
)
def test_initial_gain_failure_restores_saved_channels_and_refuses_service(
    audiod: AudioService, failure: str
) -> None:
    levels = audiod.work / "levels"
    result = run_process(
        [str(audiod.executable), "--dir", str(audiod.work)],
        name="reject unavailable audio hardware reference",
        timeout=3,
        env={
            "FPLINUX_AUDIOD_FAKE_GAIN_ERROR": failure,
            "FPLINUX_AUDIOD_FAKE_LEVELS": str(levels),
            "FPLINUX_AUDIOD_FAKE_EVENTS": str(audiod.events),
        },
        check=False,
    )
    assert result.returncode == 1
    assert "initialize the sound card:" in result.stderr
    assert not (audiod.work / "stream.sock").exists()
    assert levels.read_text(encoding="ascii") == "6 3 7\n"
    events = [event for _, event in audiod.card_events()]
    if failure == "write":
        assert "headphone volume 9 9" in events
        assert "headphone volume 3 7" in events
    else:
        assert events == []


def test_qin_fixed_speaker_uses_master_maximum_and_restores_original(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        state = audiod.status()
        assert (state["rate"], state["speaker"], state["headphones"]) == (44100, 1, 0)
        _, client, _, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 4800)
        audiod.wait(lambda: ring.field(HEARD) == 4800, "Qin native path did not drain")
        assert "hw 44100 2 441 1323" in [event for _, event in audiod.card_events()]
        assert "pcm master 7" in [event for _, event in audiod.card_events()]

    audiod.run(exercise, FPLINUX_AUDIOD_FAKE_QIN="1")
    assert audiod.card_events()[-1][1] == "master final 3"


def test_missing_speaker_profile_keeps_generic_headphone_pcm(
    audiod: AudioService,
) -> None:
    def exercise() -> None:
        state = audiod.status()
        assert (state["speaker"], state["headphones"], state["applied"]) == (0, 1, 2)
        _, client, _, stream, ring = audiod.playing_stream()
        audiod.write(client, stream, ring, 0, 4800)
        audiod.wait(lambda: ring.field(HEARD) == 4800, "Generic headphone PCM did not drain")
        assert audiod.played_left()[:4800] == [sample(frame) for frame in range(4800)]
        events = [event for _, event in audiod.card_events()]
        assert "open default" in events
        assert "hw 48000 2 480 1440" in events
        assert "pcm levels 6 6 6" in events

    audiod.run(
        exercise,
        FPLINUX_AUDIOD_FAKE_NO_PROFILE="1",
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Real audio service and application ring for the Clock signal process tests."""

from __future__ import annotations

import mmap
import os
import socket
import struct
import time
from contextlib import contextmanager
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from collections.abc import Callable, Iterator
    from pathlib import Path

MESSAGE = struct.Struct("=IIIIIIiI")


class ApplicationAudio:
    """Produce constant mono frames through an audiod-created shared ring."""

    def __init__(self, client: socket.socket, descriptor: int, stream: int) -> None:
        """Map the service's descriptor and take ownership of the mapping."""
        self.client = client
        self.stream = stream
        try:
            self.ring = mmap.mmap(descriptor, os.fstat(descriptor).st_size)
        finally:
            os.close(descriptor)
        self.capacity: int
        _, rate, channels, self.capacity = struct.unpack_from("=4I", self.ring)
        assert (rate, channels) == (48000, 1)
        self.written = 0

    def field(self, offset: int) -> int:
        """Read a published ring counter or state."""
        value: int = struct.unpack_from("=I", self.ring, offset)[0]
        return value

    def refill(self, *, level: int = 500) -> None:
        """Fill every currently free ring position at one known level."""
        room = self.capacity - (self.written - self.field(20))
        for _ in range(room):
            position = 64 + (self.written & (self.capacity - 1)) * 2
            struct.pack_into("=h", self.ring, position, level)
            self.written += 1
        struct.pack_into("=I", self.ring, 16, self.written)
        if self.field(32):
            self.client.send(MESSAGE.pack(6, 0, self.stream, 0, 0, 0, 0, 0))


class ClockAudioService:
    """Own one production daemon process with a fake playback card."""

    def __init__(self, executable: Path, work: Path) -> None:
        """Keep the executable and the caller-owned run directory explicit."""
        self.executable = executable
        self.work = work
        self.events = work / "events"
        self.played = work / "played"
        self.busy = work / "busy"
        self.process: subprocess.Popen[str] | None = None

    def run(self, exercise: Callable[[], None]) -> subprocess.CompletedProcess[str]:
        """Run a fresh daemon, exercise it after readiness, and always stop it."""

        def interact(process: subprocess.Popen[str], _deadline: float) -> None:
            self.process = process
            try:
                self.wait((self.work / "stream.sock").exists, "audiod did not start")
                exercise()
            finally:
                process.terminate()

        return run_process(
            [str(self.executable), "--dir", str(self.work)],
            name="run Clock signal's production audio service with fake ALSA",
            timeout=12,
            env={
                "PATH": os.defpath,
                "FPLINUX_AUDIOD_FAKE_EVENTS": str(self.events),
                "FPLINUX_AUDIOD_FAKE_PLAYED": str(self.played),
                "FPLINUX_AUDIOD_FAKE_BUSY": str(self.busy),
            },
            while_running=interact,
            check=True,
        )

    def wait(
        self,
        condition: Callable[[], bool],
        message: str,
        *,
        tick: Callable[[], None] | None = None,
        timeout: float = 3,
    ) -> None:
        """Wait for a bounded condition while optionally feeding application audio."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if tick is not None:
                tick()
            if condition():
                return
            assert self.process is None or self.process.poll() is None, "audiod exited"
            time.sleep(0.005)
        raise AssertionError(message)

    def played_frames(self) -> list[tuple[int, int]]:
        """Read raw stereo frames written to the fake ALSA card."""
        audio = self.played.read_bytes() if self.played.exists() else b""
        return [(left, right) for left, right in struct.iter_unpack("<hh", audio)]

    def card_events(self) -> list[str]:
        """Read the playback card's recorded lifetime events."""
        if not self.events.exists():
            return []
        return [
            line.split(" ", 1)[1] for line in self.events.read_text(encoding="ascii").splitlines()
        ]

    @contextmanager
    def manager(self) -> Iterator[socket.socket]:
        """Keep the authenticated manager socket alive for the exercise."""
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as control:
            control.connect(str(self.work / "control.sock"))
            yield control

    @staticmethod
    def clock(control: socket.socket, pid: int) -> None:
        """Announce the launched helper's actual credentials and allow its audio."""
        control.send(f"CLOCK {pid} {os.geteuid()}".encode())
        control.send(f"AUDIBLE {pid} 1".encode())

    @contextmanager
    def application(self, control: socket.socket) -> Iterator[ApplicationAudio]:
        """Own a normal application's socket and real service-created ring."""
        pid = os.getpid()
        control.send(f"APP {pid} {os.geteuid()} org.fplinux.test".encode())
        control.send(f"AUDIBLE {pid} 1".encode())
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.settimeout(3)
            client.connect(str(self.work / "stream.sock"))
            client.send(MESSAGE.pack(1, 1, 0, 0, 0, 0, 0, 0))
            client.send(MESSAGE.pack(2, 0, 0, 48000, 1, 0, 0, 0))
            data, descriptors, _, _ = socket.recv_fds(client, MESSAGE.size, 1)
            answer = MESSAGE.unpack(data)
            assert answer[0] == 3, answer
            assert len(descriptors) == 1
            application = ApplicationAudio(client, descriptors[0], answer[2])
            try:
                yield application
            finally:
                application.ring.close()

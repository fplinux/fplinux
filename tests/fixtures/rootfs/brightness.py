# SPDX-License-Identifier: GPL-2.0-only
"""Compiled brightness commands and per-case host process resources."""

from __future__ import annotations

import socket
import subprocess
import tempfile
import time
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator, Mapping

APORT = ROOT / "alpine/aports/fplinux-base"
INCLUDE = ROOT / "include/fplinux"
LIB = ROOT / "lib/fplinux"
CONFIG = "backlight=lcd\nlevels=0,1,2,4,7,11,16,23,32,44,63\n"


@dataclass(frozen=True)
class BrightnessBinaries:
    """Class-scoped immutable commands and their build directory."""

    directory: Path
    daemon: Path
    cli: Path


@dataclass
class BrightnessProcesses:
    """Test-owned sysfs, runtime files, clients and a daemon process for one case."""

    daemon: Path
    cli: Path
    work: Path
    config: Path
    backlight: Path
    socket_path: Path
    state: Path
    cleanup: ExitStack
    process: subprocess.Popen[str] | None = None

    def start_daemon(
        self, *, binary: Path | None = None, env: Mapping[str, str] | None = None
    ) -> None:
        """Start the production daemon and wait for a GET response."""
        self.process = subprocess.Popen(
            [
                str(binary or self.daemon),
                "--config",
                str(self.config),
                "--backlight",
                str(self.backlight),
                "--socket",
                str(self.socket_path),
                "--state",
                str(self.state),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                _, error = self.process.communicate(timeout=1)
                pytest.fail(f"brightness daemon exited early: {error}")
            if self.socket_path.exists():
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as probe:
                    try:
                        probe.connect(str(self.socket_path))
                    except ConnectionRefusedError:
                        pass
                    else:
                        probe.settimeout(3)
                        probe.sendall(b"GET")
                        if probe.recv(32).startswith(b"LEVEL "):
                            return
            time.sleep(0.01)
        pytest.fail("brightness daemon did not listen")

    def stop_daemon(self) -> None:
        """Stop and reap the daemon if this test started one."""
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
        self.wait_for_daemon_exit()

    def wait_for_daemon_exit(self) -> subprocess.CompletedProcess[str]:
        """Require a timely exit; SIGKILL only reaps a daemon after a failed check."""
        process = self.process
        self.process = None
        if process is None:
            pytest.fail("brightness daemon did not start")
        try:
            output, error = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)
            pytest.fail("brightness daemon did not stop without SIGKILL")
        return subprocess.CompletedProcess(process.args, process.wait(), output, error)

    def run_once(self) -> subprocess.CompletedProcess[str]:
        """Run a second daemon attempt to completion."""
        return subprocess.run(
            [
                str(self.daemon),
                "--config",
                str(self.config),
                "--backlight",
                str(self.backlight),
                "--socket",
                str(self.socket_path),
                "--state",
                str(self.state),
            ],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )

    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        """Run the production CLI against this test's socket."""
        return subprocess.run(
            [str(self.cli), "--socket", str(self.socket_path), *args],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )

    def raw(self) -> str:
        """Read the test-owned backlight's current raw value."""
        return (self.backlight / "brightness").read_text(encoding="ascii")

    def client(self) -> socket.socket:
        """Open a protocol connection tied to this test's cleanup."""
        client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.cleanup.callback(client.close)
        client.settimeout(3)
        client.connect(str(self.socket_path))
        return client

    @staticmethod
    def exchange(client: socket.socket, request: str) -> str:
        """Exchange one request and response packet."""
        client.sendall(request.encode("ascii"))
        return client.recv(32).decode("ascii")


@pytest.fixture(scope="class")
def brightness_binaries() -> Iterator[BrightnessBinaries]:
    """Compile the daemon and CLI once for each consuming test class."""
    with ExitStack() as cleanup:
        build_dir = tempfile.TemporaryDirectory()
        cleanup.enter_context(build_dir)
        directory = Path(build_dir.name)
        daemon = directory / "fplinux-brightnessd"
        cli = directory / "fplinux-brightness"
        for output, sources in (
            (daemon, (APORT / "fplinux-brightnessd.c", LIB / "fplinux-cli.c")),
            (
                cli,
                (
                    APORT / "fplinux-brightness.c",
                    LIB / "fplinux-brightness-client.c",
                    LIB / "fplinux-cli.c",
                ),
            ),
        ):
            run_process(
                [
                    "cc",
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(INCLUDE),
                    *(str(source) for source in sources),
                    "-o",
                    str(output),
                ],
                name=f"compile {output.name}",
                timeout=30,
                check=True,
            )
        yield BrightnessBinaries(directory, daemon, cli)


@pytest.fixture
def brightness(brightness_binaries: BrightnessBinaries) -> Iterator[BrightnessProcesses]:
    """Create isolated files and clean up clients and the daemon before removing them."""
    with ExitStack() as cleanup:
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        work = Path(temporary.name)
        config = work / "brightness.conf"
        config.write_text(CONFIG, encoding="ascii")
        backlight = work / "lcd"
        backlight.mkdir()
        (backlight / "max_brightness").write_text("63\n", encoding="ascii")
        (backlight / "brightness").write_text("0\n", encoding="ascii")
        processes = BrightnessProcesses(
            daemon=brightness_binaries.daemon,
            cli=brightness_binaries.cli,
            work=work,
            config=config,
            backlight=backlight,
            socket_path=work / "brightness.sock",
            state=work / "brightness.state",
            cleanup=cleanup,
        )
        cleanup.callback(processes.stop_daemon)
        yield processes

# SPDX-License-Identifier: GPL-2.0-only
"""Host-process checks for the shipped brightness daemon and command."""

from __future__ import annotations

import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, ClassVar

from tests import ROOT

if TYPE_CHECKING:
    from collections.abc import Mapping

from tests.process import run_process

APORT = ROOT / "alpine/aports/fplinux-base"
INCLUDE = ROOT / "include/fplinux"
LIB = ROOT / "lib/fplinux"
LEVELS = (0, 1, 2, 4, 7, 11, 16, 23, 32, 44, 63)
CONFIG = "backlight=lcd\nlevels=0,1,2,4,7,11,16,23,32,44,63\n"


class BrightnessProcesses(unittest.TestCase):
    """Compile production C and supply test-owned sysfs and runtime files."""

    build_dir: ClassVar[tempfile.TemporaryDirectory[str]]
    daemon: ClassVar[Path]
    cli: ClassVar[Path]

    @classmethod
    def setUpClass(cls) -> None:
        """Compile the daemon and CLI once for this test class."""
        cls.build_dir = tempfile.TemporaryDirectory()
        cls.daemon = Path(cls.build_dir.name) / "fplinux-brightnessd"
        cls.cli = Path(cls.build_dir.name) / "fplinux-brightness"
        for output, sources in (
            (cls.daemon, (APORT / "fplinux-brightnessd.c", LIB / "fplinux-cli.c")),
            (
                cls.cli,
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

    @classmethod
    def tearDownClass(cls) -> None:
        """Remove the compiled host binaries."""
        cls.build_dir.cleanup()

    def setUp(self) -> None:
        """Create isolated configuration, backlight and runtime paths."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.config = self.work / "brightness.conf"
        self.config.write_text(CONFIG, encoding="ascii")
        self.backlight = self.work / "lcd"
        self.backlight.mkdir()
        (self.backlight / "max_brightness").write_text("63\n", encoding="ascii")
        (self.backlight / "brightness").write_text("0\n", encoding="ascii")
        self.socket = self.work / "brightness.sock"
        self.state = self.work / "brightness.state"
        self.process: subprocess.Popen[str] | None = None
        self.addCleanup(self.stop_daemon)

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
                str(self.socket),
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
                self.fail(f"brightness daemon exited early: {error}")
            if self.socket.exists():
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as probe:
                    try:
                        probe.connect(str(self.socket))
                    except ConnectionRefusedError:
                        pass
                    else:
                        probe.settimeout(3)
                        probe.sendall(b"GET")
                        if probe.recv(32).startswith(b"LEVEL "):
                            return
            time.sleep(0.01)
        self.fail("brightness daemon did not listen")

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
            self.fail("brightness daemon did not start")
        try:
            output, error = process.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate(timeout=3)
            self.fail("brightness daemon did not stop without SIGKILL")
        return subprocess.CompletedProcess(process.args, process.wait(), output, error)

    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        """Run the production CLI against this test's socket."""
        return subprocess.run(
            [str(self.cli), "--socket", str(self.socket), *args],
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
        client.settimeout(3)
        client.connect(str(self.socket))
        self.addCleanup(client.close)
        return client

    @staticmethod
    def exchange(client: socket.socket, request: str) -> str:
        """Exchange one request and response packet."""
        client.sendall(request.encode("ascii"))
        return client.recv(32).decode("ascii")


class FPLinuxBrightnessCliTests(BrightnessProcesses):
    """Check the brightness command's observable host behavior."""

    def test_cli_sets_all_logical_levels_and_gets_desired_level(self) -> None:
        """Every logical level writes the specified raw code and is reported by get."""
        self.start_daemon()
        self.assertEqual(self.run_cli("get").stdout, "7\n")
        self.assertEqual(self.raw(), "23\n")
        for level, raw in enumerate(LEVELS):
            with self.subTest(level=level):
                self.assertEqual(self.run_cli("set", str(level)).returncode, 0)
                self.assertEqual(self.raw(), f"{raw}\n")
                self.assertEqual(self.run_cli("get").stdout, f"{level}\n")

    def test_cli_help_and_invalid_levels_need_no_service(self) -> None:
        """Help and invalid arguments resolve before opening the socket."""
        help_result = self.run_cli("set", "--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("LEVEL", help_result.stdout)
        for args in ((), ("set",), ("set", "01"), ("set", "11"), ("set", "1.0")):
            with self.subTest(args=args):
                self.assertEqual(self.run_cli(*args).returncode, 2)

    def test_cli_reports_unavailable_service(self) -> None:
        """Without a listening daemon the command fails with a service diagnostic."""
        result = self.run_cli("set", "8")
        self.assertEqual(result.returncode, 1)
        self.assertIn("service unavailable", result.stderr)


if __name__ == "__main__":
    unittest.main()

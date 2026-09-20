# SPDX-License-Identifier: GPL-2.0-only
"""Hermetic loopback OpenSSH server for host-tool transport tests."""

from __future__ import annotations

import os
import pwd
import shutil
import signal
import socket
import subprocess
import time
from contextlib import suppress
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    from pathlib import Path

_PROCESS_TIMEOUT_SECONDS = 10
_SERVER_START_TIMEOUT_SECONDS = 5


class OpenSshServer:
    """Own one key-only sshd bound to an ephemeral loopback port."""

    def __init__(self, directory: Path, client_key: Path) -> None:
        """Describe the owned server directory and client identity."""
        self.directory = directory
        self.client_key = client_key
        self.user = pwd.getpwuid(os.getuid()).pw_name
        self.port = self._unused_loopback_port()
        self.host_key = directory / "host_ed25519"
        self.authorized_keys = directory / "authorized_keys"
        self.log = directory / "sshd.log"
        self.config = directory / "sshd_config"
        self.process: subprocess.Popen[bytes] | None = None

    @staticmethod
    def _unused_loopback_port() -> int:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            return int(listener.getsockname()[1])

    @staticmethod
    def _tool(name: str) -> str:
        path = shutil.which(name)
        if path is None:
            message = f"host-tool SSH test requires {name}"
            raise RuntimeError(message)
        return path

    def _generate_key(self, path: Path) -> None:
        result = run_process(
            [
                self._tool("ssh-keygen"),
                "-q",
                "-t",
                "ed25519",
                "-N",
                "",
                "-f",
                str(path),
            ],
            name=f"generate test key {path.name}",
            timeout=_PROCESS_TIMEOUT_SECONDS,
        )
        if result.returncode:
            message = result.stderr.strip() or f"ssh-keygen exited {result.returncode}"
            raise RuntimeError(message)

    def start(self) -> None:
        """Generate isolated keys and start the loopback daemon."""
        self.directory.mkdir(mode=0o700)
        self.client_key.unlink(missing_ok=True)
        self.client_key.with_suffix(".pub").unlink(missing_ok=True)
        self._generate_key(self.client_key)
        self._generate_key(self.host_key)
        self.authorized_keys.write_text(
            self.client_key.with_suffix(".pub").read_text(encoding="ascii"),
            encoding="ascii",
        )
        self.authorized_keys.chmod(0o600)
        self.config.write_text(
            "".join(
                (
                    "AddressFamily inet\n",
                    "ListenAddress 127.0.0.1\n",
                    f"Port {self.port}\n",
                    f"HostKey {self.host_key}\n",
                    f"PidFile {self.directory / 'sshd.pid'}\n",
                    f"AuthorizedKeysFile {self.authorized_keys}\n",
                    "StrictModes no\n",
                    "PubkeyAuthentication yes\n",
                    "PasswordAuthentication no\n",
                    "KbdInteractiveAuthentication no\n",
                    "UsePAM no\n",
                    "PermitRootLogin prohibit-password\n",
                    f"AllowUsers {self.user}\n",
                    "PrintMotd no\n",
                    "LogLevel VERBOSE\n",
                    "Subsystem sftp internal-sftp\n",
                )
            ),
            encoding="ascii",
        )
        sshd = self._tool("sshd")
        checked = run_process(
            [sshd, "-t", "-f", str(self.config)],
            name="validate loopback sshd configuration",
            timeout=_PROCESS_TIMEOUT_SECONDS,
        )
        if checked.returncode:
            raise RuntimeError(checked.stderr.strip())
        self.process = subprocess.Popen(
            [sshd, "-D", "-E", str(self.log), "-f", str(self.config)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        self._wait_until_listening()

    def _wait_until_listening(self) -> None:
        deadline = time.monotonic() + _SERVER_START_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            if self.process is not None and self.process.poll() is not None:
                detail = self.log.read_text(encoding="utf-8", errors="replace")
                message = f"loopback sshd exited before listening:\n{detail}"
                raise RuntimeError(message)
            if self.log.exists() and "Server listening on 127.0.0.1" in self.log.read_text(
                encoding="utf-8", errors="replace"
            ):
                return
            time.sleep(0.02)
        message = "loopback sshd did not start before the test deadline"
        raise RuntimeError(message)

    def host_key_line(self, alias: str) -> str:
        """Return one known-hosts entry for the generated host key."""
        key_type, key_body, *_comment = (
            self.host_key.with_suffix(".pub").read_text(encoding="ascii").split()
        )
        return f"{alias} {key_type} {key_body}\n"

    def accepted_connections(self) -> int:
        """Count TCP sessions accepted by the daemon from its owned verbose log."""
        if not self.log.exists():
            return 0
        return self.log.read_text(encoding="utf-8", errors="replace").count(
            "Connection from 127.0.0.1 port"
        )

    def disconnected_connections(self) -> int:
        """Count authenticated TCP sessions closed by their OpenSSH client."""
        if not self.log.exists():
            return 0
        marker = f"Disconnected from user {self.user} 127.0.0.1 port"
        return self.log.read_text(encoding="utf-8", errors="replace").count(marker)

    def wait_for_connections(self, expected: int, timeout: float = 3) -> None:
        """Wait until the daemon log records the expected accepted-session count."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.accepted_connections() >= expected:
                return
            time.sleep(0.02)
        message = (
            f"sshd accepted {self.accepted_connections()} connections, expected {expected}\n"
            f"{self.log.read_text(encoding='utf-8', errors='replace')}"
        )
        raise AssertionError(message)

    def wait_for_disconnections(self, expected: int, timeout: float = 3) -> None:
        """Wait until the daemon log records the expected closed-session count."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.disconnected_connections() >= expected:
                return
            time.sleep(0.02)
        message = (
            f"sshd closed {self.disconnected_connections()} connections, expected {expected}\n"
            f"{self.log.read_text(encoding='utf-8', errors='replace')}"
        )
        raise AssertionError(message)

    def stop(self) -> None:
        """Stop and reap the daemon process group owned by this fixture."""
        if self.process is None:
            return
        with suppress(ProcessLookupError):
            os.killpg(self.process.pid, signal.SIGTERM)
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            with suppress(ProcessLookupError):
                os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait()
        self.process = None

# SPDX-License-Identifier: GPL-2.0-only
"""Windows authenticated SSH transport."""

from __future__ import annotations

import hashlib
import hmac
import ipaddress
import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import is_sha256
from fplinux_cli.runtime.connection import ConnectionLostError
from fplinux_cli.runtime.ssh import commands, material

from .host import (
    UsbProbeCancelledError,
    WindowsHost,
    bound_interface,
    console_creation_flags,
    exact_usb_devices,
    fail,
)
from .session import write_session

if TYPE_CHECKING:
    from collections.abc import Callable
    from typing import BinaryIO


class WindowsTransport:
    """The packaged SSH client and its exact live USB session."""

    def __init__(self, host: WindowsHost, tools: dict[str, Path], *, wait_seconds: int) -> None:
        """Keep Windows effects and admitted executable paths at their boundaries."""
        self.host = host
        self.tools = tools
        self.wait_seconds = wait_seconds

    def _ssh_arguments(
        self, session: dict[str, Any], command: str | None = None, *, connect_timeout: int = 5
    ) -> list[str]:
        options = [
            *commands.ssh_policy_options(
                usb_serial=session["usb_serial"],
                known_hosts=str(session["known_hosts"]).replace("\\", "/"),
                host_address=session["host_address"],
                connect_timeout=connect_timeout,
            ),
            ("HostKeyAlgorithms", "ssh-ed25519"),
            ("GlobalKnownHostsFile", "NUL"),
        ]
        argv = [str(self.tools["ssh"]), "-4", "-F", "NUL", "-i", session["private_key"]]
        for name, value in options:
            rendered = commands.ssh_config_value(value)
            if name in {"IdentityAgent", "ProxyCommand", "ProxyJump"}:
                rendered = "none"
            argv.extend(["-o", f"{name}={rendered}"])
        argv.append(f"root@{session['phone_address']}")
        if command is not None:
            argv.append(command)
        return argv

    def _require_bound(self, session: dict[str, Any]) -> None:
        directory = Path(session["directory"])
        self.host.require_private(directory)
        self.host.require_private(Path(session["private_key"]))
        self.host.require_private(Path(session["known_hosts"]))
        if (
            session["status"] != "ready"
            or bound_interface(self.host.network_snapshot(session), session) is None
        ):
            fail("The exact phone USB network endpoint is no longer ready")

    def usb_present(
        self,
        session: dict[str, Any],
        *,
        cancelled: Callable[[], bool] | None = None,
    ) -> bool:
        """Observe the exact USB root independently of its current network interface."""
        try:
            snapshot = self.host.ps("devices.ps1", cancelled=cancelled)
        except UsbProbeCancelledError:
            return False
        devices = exact_usb_devices(snapshot, session["vendor_id"], session["product_id"])
        matching = [
            device
            for device in devices
            if device.rsplit("\\", 1)[-1].lower() == session["usb_serial"].lower()
        ]
        if len(matching) > 1:
            fail("Multiple USB devices match this saved RAM session")
        return bool(matching)

    def run_remote(
        self,
        session: dict[str, Any],
        command: str,
        *,
        capture_output: bool = False,
        shared: bool = False,
        timeout: float | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Run dedicated SSH; shared metadata operations leave caller input untouched."""
        self._require_bound(session)
        return subprocess.run(
            self._ssh_arguments(session, command),
            stdin=subprocess.DEVNULL if shared else None,
            capture_output=capture_output,
            text=True,
            check=False,
            timeout=timeout,
            creationflags=console_creation_flags(),
        )

    def run_shell(self, session: dict[str, Any]) -> int:
        """Retain terminal ownership and report SSH failure independently of USB discovery."""
        self._require_bound(session)
        if not os.isatty(0):
            fail("Interactive SSH requires a terminal")
        argv = self._ssh_arguments(session)
        argv[1:1] = ["-tt"]
        process = subprocess.Popen(argv, creationflags=0)
        status: int | None = None
        try:
            while status is None:
                try:
                    status = process.wait(timeout=0.25)
                except subprocess.TimeoutExpired:
                    present = self.usb_present(
                        session, cancelled=lambda: process.poll() is not None
                    )
                    status = process.poll()
                    if status is None and not present:
                        raise ConnectionLostError from None
            if status == 255:
                raise ConnectionLostError
            return status
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
        progress: Callable[[int], None] | None = None,
    ) -> None:
        """Keep binary stdout separate from diagnostics and terminate a timed-out reader."""
        if timeout <= 0:
            message = "SSH stream timeout must be positive"
            raise ValueError(message)
        self._require_bound(session)
        start = 0
        if progress is not None:
            destination.flush()
            start = os.lseek(destination.fileno(), 0, os.SEEK_CUR)
        deadline = time.monotonic() + timeout
        with tempfile.TemporaryFile(mode="w+b") as stderr:
            process = subprocess.Popen(
                self._ssh_arguments(session, command),
                stdin=subprocess.DEVNULL,
                stdout=destination,
                stderr=stderr,
                creationflags=console_creation_flags(),
            )
            try:
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        fail(f"SSH remote command timed out after {timeout:g}s")
                    try:
                        status = process.wait(
                            timeout=remaining if progress is None else min(0.1, remaining)
                        )
                        break
                    except subprocess.TimeoutExpired:
                        if progress is not None:
                            progress(
                                max(0, os.lseek(destination.fileno(), 0, os.SEEK_CUR) - start)
                            )
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                if progress is not None:
                    progress(max(0, os.lseek(destination.fileno(), 0, os.SEEK_CUR) - start))
            if status:
                detail = commands.stream_stderr_detail(stderr)
                fail(
                    f"SSH remote command failed with exit status {status}"
                    + (f": {detail}" if detail else "")
                )

    def authenticate(
        self,
        session: dict[str, Any],
        *,
        expected_device_identity: str,
        timeout: float,
        scan_host_key: bool = True,
    ) -> None:
        """Pin a key only after USB binding, then verify RAM and kernel identities."""
        deadline = time.monotonic() + timeout
        pinned = not scan_host_key

        def current_interface() -> int | None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            snapshot = self.host.network_snapshot(session, timeout=min(20, remaining))
            return bound_interface(snapshot, session)

        while time.monotonic() < deadline:
            try:
                index = current_interface()
            except subprocess.TimeoutExpired:
                continue
            if index is None:
                time.sleep(0.5)
                continue
            try:
                if not pinned:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        continue
                    scan = subprocess.run(
                        [
                            str(self.tools["keyscan"]),
                            "-4",
                            "-T",
                            "3",
                            "-t",
                            "ed25519",
                            session["phone_address"],
                        ],
                        capture_output=True,
                        text=True,
                        timeout=min(5, remaining),
                        check=False,
                        creationflags=console_creation_flags(),
                    )
                    keys = {
                        tuple(fields[1:])
                        for line in scan.stdout.splitlines()
                        if len(fields := line.split()) == 3
                        and fields[0] == session["phone_address"]
                    }
                    if scan.returncode or len(keys) != 1:
                        time.sleep(0.5)
                        continue
                    key_type, key_body = next(iter(keys))
                    known_hosts = Path(session["known_hosts"])
                    known_hosts.write_text(f"{key_type} {key_body}\n", encoding="ascii")
                    material.public_key_body(known_hosts)
                    known_hosts.write_text(
                        f"fplinux-{session['usb_serial']} {key_type} {key_body}\n",
                        encoding="ascii",
                    )
                    self.host.secure_new(known_hosts)
                    pinned = True
                if current_interface() != index:
                    continue
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    continue
                connect_timeout = max(1, min(5, int(remaining)))
                result = subprocess.run(
                    self._ssh_arguments(
                        session, "fplinux-session-id; uname -r", connect_timeout=connect_timeout
                    ),
                    stdin=subprocess.DEVNULL,
                    capture_output=True,
                    text=True,
                    timeout=min(connect_timeout + 1, remaining),
                    check=False,
                    creationflags=console_creation_flags(),
                )
            except subprocess.TimeoutExpired:
                continue
            if result.returncode:
                time.sleep(0.5)
                continue
            lines = result.stdout.splitlines()
            if len(lines) != 2 or not hmac.compare_digest(lines[0], session["session_id"]):
                fail("The authenticated phone returned another RAM session identity")
            if not lines[1].endswith(f"-fplinux-{expected_device_identity[:16]}"):
                fail("The authenticated phone returned another kernel build identity")
            try:
                if current_interface() != index:
                    continue
            except subprocess.TimeoutExpired:
                continue
            if session["status"] != "ready":
                session["status"] = "ready"
                write_session(self.host, Path(session["directory"]), session)
            return
        fail(
            "The exact USB RNDIS and authenticated SSH endpoint "
            "did not become ready before the deadline"
        )

    def _validate_saved_session(
        self, value: object, directory: Path, *, target: str
    ) -> dict[str, Any]:
        required = {
            "target",
            "session_id",
            "usb_serial",
            "network",
            "host_address",
            "phone_address",
            "host_mac",
            "device_mac",
            "private_key",
            "known_hosts",
            "image",
            "directory",
            "vendor_id",
            "product_id",
            "generation",
            "device_identity",
            "status",
            "prepared_data_sha256",
        }
        if not isinstance(value, dict) or set(value) != required or value.get("status") != "ready":
            fail("Saved phone connection is not ready")
        session_id = value["session_id"]
        if (
            value["target"] != target
            or not isinstance(value["generation"], str)
            or not is_sha256(value["generation"])
            or not isinstance(value["device_identity"], str)
            or not is_sha256(value["device_identity"])
            or (
                value["prepared_data_sha256"] is not None
                and (
                    not isinstance(value["prepared_data_sha256"], str)
                    or not is_sha256(value["prepared_data_sha256"])
                )
            )
            or not isinstance(session_id, str)
            or not is_sha256(session_id)
            or value["usb_serial"] != hashlib.sha256(bytes.fromhex(session_id)).hexdigest()[:32]
        ):
            fail("Saved phone connection belongs to another archive or RAM session")
        try:
            network = ipaddress.IPv4Network(value["network"], strict=True)
        except TypeError, ValueError:
            fail("Saved phone connection has an invalid network")
        if network.prefixlen != 30 or not material.is_rfc1918(network):
            fail("Saved phone connection does not use a private /30 network")
        addresses = tuple(str(address) for address in network.hosts())
        if (value["host_address"], value["phone_address"]) != addresses:
            fail("Saved phone connection addresses do not match its network")
        macs = (value["host_mac"], value["device_mac"])
        if (
            any(
                not isinstance(mac, str)
                or re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", mac) is None
                or int(mac[:2], 16) & 3 != 2
                for mac in macs
            )
            or macs[0] == macs[1]
        ):
            fail("Saved phone connection has invalid private USB addresses")
        if any(
            type(value[field]) is not int or not 0 <= value[field] <= 0xFFFF
            for field in ("vendor_id", "product_id")
        ):
            fail("Saved phone connection has an invalid USB identity")
        if directory.name != value["usb_serial"] or value["directory"] != str(directory):
            fail("Saved phone connection directory does not match its RAM identity")
        for field, name in (
            ("private_key", "id_ed25519"),
            ("known_hosts", "known_hosts"),
            ("image", "image.bin"),
        ):
            if value[field] != str(directory / name):
                fail("Saved phone connection contains a path outside its directory")
        for path in (
            directory,
            directory / "session.json",
            Path(value["private_key"]),
            Path(value["known_hosts"]),
        ):
            self.host.require_private(path)
        return value

    def load_saved_sessions(self, target: str, *, session_root: Path) -> list[dict[str, Any]]:
        """Read ready bindings without requiring a connected USB device or SSH peer."""
        root = self.host.runtime_root(session_root)
        candidates: list[dict[str, Any]] = []
        for state_path in root.glob("*/session.json"):
            try:
                value = json.loads(state_path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                fail(f"Saved phone connection cannot be read: {error}")
            if isinstance(value, dict) and value.get("status") == "prepared":
                continue
            candidates.append(
                self._validate_saved_session(value, state_path.parent, target=target)
            )
        return candidates

    def load_saved_session(
        self,
        target: str,
        *,
        session_root: Path,
    ) -> dict[str, Any] | None:
        """Read one ready binding without requiring a connected USB device or SSH peer."""
        candidates = self.load_saved_sessions(target, session_root=session_root)
        if not candidates:
            return None
        if len(candidates) > 1:
            fail("Multiple saved phone connections cannot select one RAM session")
        return candidates[0]

    def reacquire_bound_session(
        self, session: dict[str, Any], *, expected_device_identity: str | None = None
    ) -> dict[str, Any]:
        """Authenticate a reappearing RAM session against its existing pinned host key."""
        if (
            expected_device_identity is not None
            and expected_device_identity != session["device_identity"]
        ):
            fail("The requested connection belongs to another kernel build identity")
        self.authenticate(
            session,
            expected_device_identity=session["device_identity"],
            timeout=self.wait_seconds,
            scan_host_key=False,
        )
        return session

    def retire_previous_sessions(self, current: dict[str, Any]) -> None:
        """Retain one ready connection while leaving unrecognized files and records intact."""
        root = Path(current["directory"]).parent
        for state_path in root.glob("*/session.json"):
            directory = state_path.parent
            if directory == Path(current["directory"]):
                continue
            try:
                value = json.loads(state_path.read_text(encoding="utf-8"))
                self._validate_saved_session(value, directory, target=current["target"])
            except OSError, ValueError, SystemExit:
                continue
            files = [
                directory / name
                for name in (
                    "id_ed25519",
                    "id_ed25519.pub",
                    "known_hosts",
                    "session.json",
                )
            ]
            for path in files:
                if path.exists():
                    self.host.require_private(path)
            for path in files:
                path.unlink(missing_ok=True)
            if not any(directory.iterdir()):
                directory.rmdir()

# SPDX-License-Identifier: GPL-2.0-only
"""Linux USB discovery and authenticated SSH transport."""

from __future__ import annotations

import base64
import binascii
import contextlib
import hmac
import json
import math
import os
import signal
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import is_sha256, sha256_file
from fplinux_cli.runtime.connection import (
    BOOT_IDENTITY_COMMAND,
    ConnectionLostError,
    observed_device_identity,
)

from .commands import (
    SSH_PORT,
    remote_path,
    require_tool,
    run_with_input,
    ssh_argv,
    ssh_options,
    stream_stderr_detail,
    upload_publication,
    upload_space_check,
)
from .errors import fail
from .session import mark_current, validate_session

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import FrameType
    from typing import IO, BinaryIO


def _usb_devices(session: dict[str, Any], *, require_absence_proof: bool = False) -> list[Path]:
    """Find the exact USB identity, distinguishing failed discovery for a live probe."""
    matches: list[Path] = []
    root = Path("/sys/bus/usb/devices")
    try:
        devices = (
            root.iterdir()
            if require_absence_proof
            else (vendor_file.parent for vendor_file in root.glob("*/idVendor"))
        )
        for device in devices:
            vendor_file = device / "idVendor"
            try:
                vendor = int(vendor_file.read_text(encoding="ascii").strip(), 16)
                if vendor != session["vendor_id"]:
                    continue
                product = int((device / "idProduct").read_text(encoding="ascii").strip(), 16)
                if product != session["product_id"]:
                    continue
                serial = (device / "serial").read_text(encoding="ascii").strip()
            except FileNotFoundError as error:
                # USB interfaces have no idVendor; a matching device with unreadable
                # endpoint attributes cannot establish physical absence.
                if require_absence_proof and vendor_file.exists():
                    fail(f"cannot discover the current USB session: {error}")
                continue
            except (PermissionError, UnicodeDecodeError, ValueError) as error:
                if require_absence_proof:
                    fail(f"cannot discover the current USB session: {error}")
                continue
            if hmac.compare_digest(serial, session["usb_serial"]):
                matches.append(device.resolve(strict=require_absence_proof))
    except OSError as error:
        if require_absence_proof:
            fail(f"cannot discover the current USB session: {error}")
        raise
    return matches


def _driver_is_cdc_ncm(path: Path, usb_device: Path) -> bool:
    current = path
    while current != usb_device.parent:
        driver = current / "driver"
        try:
            if driver.resolve().name == "cdc_ncm":
                return True
        except FileNotFoundError, OSError:
            pass
        if current == usb_device:
            break
        current = current.parent
    return False


def _ncm_interface(usb_device: Path, expected_mac: str) -> str | None:
    matches: list[str] = []
    for netdev in Path("/sys/class/net").iterdir():
        try:
            device = (netdev / "device").resolve(strict=True)
            address = (netdev / "address").read_text(encoding="ascii").strip().lower()
        except FileNotFoundError, PermissionError, UnicodeDecodeError, OSError:
            continue
        if usb_device not in (device, *device.parents):
            continue
        if address != expected_mac or not _driver_is_cdc_ncm(device, usb_device):
            continue
        matches.append(netdev.name)
    if len(matches) > 1:
        fail("more than one matching cdc_ncm interface belongs to the RAM session")
    return matches[0] if matches else None


def _ip_json(arguments: list[str]) -> object | None:
    result = subprocess.run(
        [require_tool("ip"), "-4", "-j", *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        return None
    try:
        decoded: object = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None
    return decoded


def _network_ready(interface: str, session: dict[str, Any]) -> bool:
    addresses = _ip_json(["address", "show", "dev", interface])
    if not isinstance(addresses, list) or len(addresses) != 1:
        return False
    infos = addresses[0].get("addr_info") if isinstance(addresses[0], dict) else None
    if not isinstance(infos, list) or not any(
        isinstance(info, dict)
        and info.get("family") == "inet"
        and info.get("local") == session["host_address"]
        and info.get("prefixlen") == 30
        for info in infos
    ):
        return False
    routes = _ip_json(["route", "show", session["network"]])
    if not isinstance(routes, list):
        return False
    return any(
        isinstance(route, dict)
        and route.get("dst") == session["network"]
        and route.get("dev") == interface
        for route in routes
    )


def _scan_host_key(session: dict[str, Any], timeout: int) -> bool:
    scanner = require_tool("ssh-keyscan")
    result = subprocess.run(
        [
            scanner,
            "-4",
            "-T",
            str(timeout),
            "-p",
            str(SSH_PORT),
            "-t",
            "ed25519",
            session["phone_address"],
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    lines = [line for line in result.stdout.splitlines() if line and not line.startswith("#")]
    keys = {tuple(line.split()[1:3]) for line in lines if len(line.split()) == 3}
    if result.returncode or len(keys) != 1:
        return False
    key_type, key_body = next(iter(keys))
    if key_type != "ssh-ed25519":
        return False
    try:
        decoded = base64.b64decode(key_body, validate=True)
    except binascii.Error:
        return False
    if (
        len(decoded) != 51
        or decoded[:15] != b"\0\0\0\x0bssh-ed25519"
        or decoded[15:19] != b"\0\0\0\x20"
    ):
        return False
    alias = f"fplinux-{session['usb_serial']}"
    known_hosts = Path(session["known_hosts"])
    known_hosts.write_text(f"{alias} {key_type} {key_body}\n", encoding="ascii")
    known_hosts.chmod(0o600)
    return True


def _retry_pause(deadline: float) -> None:
    remaining = deadline - time.monotonic()
    if remaining > 0:
        time.sleep(min(0.25, remaining))


def _wait_for_authenticated_endpoint(
    session: dict[str, Any],
    *,
    deadline: float,
    scan_host_key: bool,
    on_linux_usb: Callable[[], None] | None = None,
) -> dict[str, Any]:
    """Wait for the exact USB path and an authenticated session-id response."""
    announced_key = False
    announced_usb = False
    while time.monotonic() < deadline:
        devices = _usb_devices(session)
        if len(devices) > 1:
            fail("more than one USB device matches this RAM session")
        if devices and not announced_usb:
            if on_linux_usb is not None:
                on_linux_usb()
            announced_usb = True
        interface = _ncm_interface(devices[0], session["host_mac"]) if devices else None
        if interface is None or not _network_ready(interface, session):
            _retry_pause(deadline)
            continue

        remaining_seconds = max(1, math.ceil(deadline - time.monotonic()))
        timeout = min(5, remaining_seconds)
        candidate = {**session, "interface": interface}
        if scan_host_key:
            if not _scan_host_key(candidate, timeout):
                _retry_pause(deadline)
                continue
            if not announced_key:
                print(
                    "SSH host key observed; validating the private RAM session identity.",
                    file=sys.stderr,
                    flush=True,
                )
                announced_key = True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            # ConnectTimeout bounds connection setup, not the remote identity command.
            result = subprocess.run(
                ssh_argv(candidate, "fplinux-session-id", connect_timeout=timeout),
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=min(5, remaining),
                check=False,
            )
        except subprocess.TimeoutExpired:
            _retry_pause(deadline)
            continue
        if result.returncode:
            _retry_pause(deadline)
            continue
        actual = result.stdout.strip()
        if not hmac.compare_digest(actual, session["session_id"]):
            fail("SSH endpoint proved a different RAM session identity")
        return {**candidate, "status": "ready"}
    if scan_host_key:
        return fail("the exact USB-NCM SSH session did not become ready before the deadline")
    return fail("the current USB-NCM SSH session did not reconnect before the deadline")


def wait_for_bound_session(
    session: dict[str, Any],
    *,
    expected_device_identity: str,
    on_linux_usb: Callable[[], None] | None = None,
) -> dict[str, Any]:
    """Wait for exact USB/NCM state, then bind the observed host key to this RAM run."""
    return _authenticate_bound_session(
        session,
        expected_device_identity=expected_device_identity,
        scan_host_key=True,
        on_linux_usb=on_linux_usb,
    )


def _authenticate_bound_session(
    session: dict[str, Any],
    *,
    expected_device_identity: str | None,
    scan_host_key: bool,
    on_linux_usb: Callable[[], None] | None = None,
) -> dict[str, Any]:
    """Authenticate both identities and publish credentials only for a new RAM session."""
    deadline = time.monotonic() + session["wait_seconds"]
    candidate = _wait_for_authenticated_endpoint(
        session, deadline=deadline, scan_host_key=scan_host_key, on_linux_usb=on_linux_usb
    )
    remaining = deadline - time.monotonic()
    timeout_message = (
        "the exact USB-NCM SSH session did not become ready before the deadline"
        if scan_host_key
        else "the current USB-NCM SSH session did not reconnect before the deadline"
    )
    if remaining <= 0:
        fail(timeout_message)
    if expected_device_identity is not None:
        try:
            require_device_identity(candidate, expected_device_identity, timeout=remaining)
        except subprocess.TimeoutExpired:
            fail(timeout_message)
    if scan_host_key:
        return mark_current(candidate)
    return candidate


def usb_present(session: dict[str, Any], *, cancelled: Callable[[], bool] | None = None) -> bool:
    """Confirm exact USB presence; ambiguous or failed discovery remains an error."""
    if cancelled is not None and cancelled():
        return False
    devices = _usb_devices(session, require_absence_proof=True)
    if len(devices) > 1:
        fail("more than one USB device matches this RAM session")
    return bool(devices)


def reacquire_bound_session(
    session: dict[str, Any], *, expected_device_identity: str | None
) -> dict[str, Any]:
    """Survive a physical replug without weakening USB, host-key or session binding."""
    session = validate_session(session)
    return _authenticate_bound_session(
        session, expected_device_identity=expected_device_identity, scan_host_key=False
    )


def run_shell(session: dict[str, Any]) -> int:
    """Run interactive SSH with inherited terminal streams and reap it on cancellation."""
    session = validate_session(session)
    if not os.isatty(0):
        fail("interactive SSH requires a terminal; use --exec for a non-interactive command")
    argv = ssh_argv(session)
    argv[1:1] = ["-tt"]
    process = subprocess.Popen(argv)
    interrupted: int | None = None

    def forward_signal(signum: int, _frame: FrameType | None) -> None:
        nonlocal interrupted
        interrupted = signum
        with contextlib.suppress(ProcessLookupError):
            process.send_signal(signum)

    previous_handlers = {
        signum: signal.signal(signum, forward_signal)
        for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)
    }
    try:
        while process.poll() is None and interrupted is None:
            try:
                process.wait(timeout=0.25)
            except subprocess.TimeoutExpired:
                if interrupted is None and process.poll() is None and not usb_present(session):
                    raise ConnectionLostError from None
    finally:
        try:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
    if interrupted is not None:
        if interrupted == signal.SIGINT:
            raise KeyboardInterrupt
        raise SystemExit(128 + interrupted)
    status = process.wait()
    if status == 255:
        raise ConnectionLostError
    return status


def run_remote(
    session: dict[str, Any],
    command: str,
    *,
    capture_output: bool = False,
    shared: bool = False,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    """Run a command; shared metadata operations leave caller input untouched."""
    session = validate_session(session)
    return subprocess.run(
        ssh_argv(session, command),
        stdin=subprocess.DEVNULL if shared else None,
        capture_output=capture_output,
        text=True,
        timeout=timeout,
        check=False,
    )


def send_remote(
    session: dict[str, Any], command: str, write: Callable[[IO[bytes]], None]
) -> subprocess.CompletedProcess[str]:
    """Send input to one authenticated SSH command and capture its result."""
    return run_with_input(ssh_argv(validate_session(session), command), write)


def require_device_identity(
    session: dict[str, Any], device_identity: str, *, timeout: float | None = None
) -> str:
    """Require the authenticated phone to expose the selected kernel and boot recipe."""
    if not is_sha256(device_identity):
        fail("expected device identity is invalid")
    result = run_remote(
        session, BOOT_IDENTITY_COMMAND, capture_output=True, shared=True, timeout=timeout
    )
    if result.returncode:
        detail = result.stderr.strip().splitlines()
        fail(
            f"cannot read the running kernel identity (exit {result.returncode})"
            + (f": {detail[-1]}" if detail else "")
        )
    lines = result.stdout.splitlines()
    if observed_device_identity(lines) != device_identity:
        actual = lines[0] if lines else "no kernel release"
        fail(
            "current SSH session exposes a different kernel identity or boot recipe\n"
            f"  phone:  {actual}"
        )
    return lines[0]


def _terminate_stream_process(process: subprocess.Popen[bytes]) -> None:
    """Stop one isolated SSH process group and wait for every child it owns."""
    if process.poll() is not None:
        return
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def stream_remote(
    session: dict[str, Any],
    command: str,
    destination: BinaryIO,
    *,
    timeout: float,
    progress: Callable[[int], None] | None = None,
) -> None:
    """Stream one authenticated SSH command directly to an already-open local file."""
    if timeout <= 0:
        message = "SSH stream timeout must be positive"
        raise ValueError(message)
    session = validate_session(session)
    initial_position = 0
    if progress is not None:
        destination.flush()
        initial_position = os.lseek(destination.fileno(), 0, os.SEEK_CUR)
    interrupted: int | None = None
    with tempfile.TemporaryFile(mode="w+b") as stderr:
        process = subprocess.Popen(
            ssh_argv(session, command),
            stdin=subprocess.DEVNULL,
            stdout=destination,
            stderr=stderr,
            start_new_session=True,
        )

        def forward_signal(signum: int, _frame: FrameType | None) -> None:
            nonlocal interrupted
            interrupted = signum
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signum)

        previous_handlers = {
            signum: signal.signal(signum, forward_signal)
            for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)
        }
        timed_out = False
        try:
            deadline = time.monotonic() + timeout
            while process.poll() is None and interrupted is None:
                if progress is not None:
                    progress(os.lseek(destination.fileno(), 0, os.SEEK_CUR) - initial_position)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    break
                try:
                    process.wait(timeout=min(remaining, 0.25))
                except subprocess.TimeoutExpired:
                    continue
            if process.poll() is None:
                _terminate_stream_process(process)
            if progress is not None:
                progress(os.lseek(destination.fileno(), 0, os.SEEK_CUR) - initial_position)
        except BaseException:
            _terminate_stream_process(process)
            raise
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)

        detail = stream_stderr_detail(stderr)
        if interrupted is not None:
            raise SystemExit(128 + interrupted)
        if timed_out:
            fail(f"SSH remote command timed out after {timeout:g}s")
        if process.returncode:
            message = f"SSH remote command failed with exit status {process.returncode}"
            fail(message + (f": {detail}" if detail else ""))


def _sftp_quote(value: str) -> str:
    if any(character in value for character in ("\0", "\n", "\r")):
        fail("SFTP path contains a line separator")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _sftp(session: dict[str, Any], command: str) -> subprocess.CompletedProcess[str]:
    argv = [
        require_tool("sftp"),
        "-S",
        require_tool("ssh"),
        "-q",
        "-b",
        "-",
        *ssh_options(session),
        f"root@{session['phone_address']}",
    ]
    return subprocess.run(
        argv,
        input=command + "\n",
        capture_output=True,
        text=True,
        check=False,
    )


def _remote_metadata(session: dict[str, Any], remote: str) -> tuple[int, str]:
    command = (
        f"if [ -f '{remote}' ] && [ -r '{remote}' ]; then "
        f"set -- $(sha256sum '{remote}'); size=$(wc -c < '{remote}'); "
        'printf "%s %s\\n" "$size" "$1"; else exit 44; fi'
    )
    result = run_remote(session, command, capture_output=True, shared=True)
    if result.returncode:
        fail(f"the phone cannot read {remote}")
    fields = result.stdout.strip().split()
    if len(fields) != 2 or not fields[0].isdigit() or not is_sha256(fields[1]):
        fail("the phone returned invalid file metadata")
    return int(fields[0]), fields[1]


def upload(session: dict[str, Any], local_name: str, remote_name: str) -> None:
    """Upload through SFTP, then verify and atomically publish on the phone."""
    session = validate_session(session)
    remote = remote_path(remote_name, "upload destination")
    local = Path(local_name)
    try:
        metadata = local.lstat()
    except OSError as error:
        fail(f"cannot inspect upload source {local}: {error}")
    if not stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        fail(f"upload source is not a regular file: {local}")
    expected_hash = sha256_file(local)
    expected_size = metadata.st_size
    directory = remote.rsplit("/", 1)[0] or "/"
    check = run_remote(
        session,
        upload_space_check(directory, expected_size),
        capture_output=True,
        shared=True,
    )
    if check.returncode:
        fail("upload destination is missing or does not have enough free space")
    nonce = os.getrandom(12).hex()
    temporary = f"{directory.rstrip('/')}/.fplinux-upload.{nonce}"
    if directory == "/":
        temporary = f"/.fplinux-upload.{nonce}"
    published = False
    try:
        result = _sftp(
            session,
            f"put {_sftp_quote(str(local))} {_sftp_quote(temporary)}",
        )
        if result.returncode:
            detail = result.stderr.strip().splitlines()
            fail("SFTP upload failed" + (f": {detail[-1]}" if detail else ""))
        if sha256_file(local) != expected_hash or local.stat().st_size != expected_size:
            fail(f"upload source changed while it was sent: {local}")
        publish = run_remote(
            session,
            upload_publication(temporary, remote, expected_hash, expected_size),
            capture_output=True,
            shared=True,
        )
        if publish.returncode:
            fail("device-side SHA-256 verification or atomic upload publication failed")
        published = True
    finally:
        if not published:
            run_remote(session, f"rm -f '{temporary}'", capture_output=True, shared=True)
    print(
        f"upload verified: {local} ({expected_size} bytes, sha256={expected_hash}) -> {remote}",
        flush=True,
    )


def pull(session: dict[str, Any], remote_name: str, local_name: str) -> None:
    """Download through SFTP and publish locally only after end-to-end verification."""
    session = validate_session(session)
    remote = remote_path(remote_name, "pull source")
    expected_size, expected_hash = _remote_metadata(session, remote)
    destination = Path(local_name)
    parent = destination.parent
    if parent.is_symlink() or not parent.is_dir():
        fail(f"pull destination directory is missing or invalid: {parent}")
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
        temporary.chmod(0o600)
        result = _sftp(
            session,
            f"get {_sftp_quote(remote)} {_sftp_quote(str(temporary))}",
        )
        if result.returncode:
            detail = result.stderr.strip().splitlines()
            fail("SFTP pull failed" + (f": {detail[-1]}" if detail else ""))
        actual_size = temporary.stat().st_size
        actual_hash = sha256_file(temporary)
        if actual_size != expected_size or actual_hash != expected_hash:
            fail("downloaded file does not match the size and SHA-256 reported by the phone")
        if _remote_metadata(session, remote) != (expected_size, expected_hash):
            fail("pull source changed while it was downloaded")
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    message = f"pull verified: {remote} ({expected_size} bytes, sha256={expected_hash})"
    print(f"{message} -> {destination}", flush=True)

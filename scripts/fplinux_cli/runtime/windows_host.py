# SPDX-License-Identifier: GPL-2.0-only
"""Load one archive on Windows and bind its RNDIS endpoint to authenticated SSH."""

from __future__ import annotations

import hashlib
import hmac
import ipaddress
import json
import os
import platform
import re
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

from . import bundle as bundle_runtime
from . import ssh_transport
from .connection import ConnectionLostError
from .windows_tools import WINDOWS_TOOLS, windows_tool_paths

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import ModuleType
    from typing import BinaryIO

_USB_READY_MARKERS = (b"FPLINUX_BOOTROM_READY\n", b"FPLINUX_BOOTROM_READY\r\n")


def _console_creation_flags() -> int:
    return int(getattr(subprocess, "CREATE_NO_WINDOW", 0))


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FPLinux: {message}")


class BootromDriverRequiredError(Exception):
    """The native loader could not open or claim an unsupported USB driver."""


class _UsbProbeCancelledError(Exception):
    """The owner stopped a read-only Windows device probe."""


def _cancellable_discovery(
    argv: list[str], *, timeout: float, cancelled: Callable[[], bool]
) -> subprocess.CompletedProcess[str]:
    """Stop and reap discovery when its USB observer no longer owns the result."""
    if cancelled():
        raise _UsbProbeCancelledError
    process = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        creationflags=_console_creation_flags(),
    )
    deadline = time.monotonic() + timeout
    try:
        while True:
            if cancelled():
                raise _UsbProbeCancelledError
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(argv, timeout)
            try:
                stdout, stderr = process.communicate(timeout=min(0.1, remaining))
            except subprocess.TimeoutExpired:
                continue
            return subprocess.CompletedProcess(argv, process.wait(), stdout, stderr)
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate()


def _wait_usb_loader(  # noqa: PLR0913 -- native progress and reconnect observers stay explicit.
    process: subprocess.Popen[bytes],
    *,
    timeout: int,
    log: Path,
    offset: int,
    on_usb_ready: Callable[[], None],
    on_wait: Callable[[], None] | None,
) -> int:
    """Observe this child's USB acquisition without delaying its native discovery."""
    deadline = time.monotonic() + timeout
    pending = b""
    suffix_length = max(len(marker) for marker in _USB_READY_MARKERS) - 1
    with log.open("rb") as stream:
        stream.seek(offset)
        while True:
            status = process.poll()
            remaining = deadline - time.monotonic()
            if status is None and remaining <= 0:
                raise subprocess.TimeoutExpired(process.args, timeout)
            chunk = stream.read(65536)
            if chunk:
                pending += chunk
                if any(marker in pending for marker in _USB_READY_MARKERS):
                    on_usb_ready()
                    if process.stdin is None:
                        fail("USB loader acknowledgement pipe is unavailable")
                    with suppress(BrokenPipeError):
                        process.stdin.write(b"\x06")
                        process.stdin.flush()
                    return process.wait(timeout=max(0, deadline - time.monotonic()))
                pending = pending[-suffix_length:]
                continue
            if status is not None:
                if status == 2:
                    raise BootromDriverRequiredError
                if status == 0:
                    fail("The RAM loader exited without confirming its USB connection")
                return status
            if on_wait is not None:
                on_wait()
            with suppress(subprocess.TimeoutExpired):
                process.wait(timeout=min(0.1, remaining))


@dataclass(frozen=True)
class WindowsBundle:
    """The validated canonical image and its separately packaged native tools."""

    root: Path
    runtime: dict[str, Any]
    tools: dict[str, Path]
    adapter: ModuleType
    ssh: ModuleType
    generation: str
    device_identity: str


def validate_archive(
    root: Path, runtime: dict[str, Any], *, generation: str, device_identity: str
) -> WindowsBundle:
    """Check files without discovering USB or creating private host state."""
    runner = bundle_runtime
    if runtime["transport"] != "usb-ncm":
        fail("This archive does not support the authenticated Linux endpoint")
    tool_paths = windows_tool_paths(runtime["host_tools"])
    platform_name = runtime["identity"]["platform"]["name"]
    native_roles = {"loader", "bridge"} if platform_name == "ums9117" else {"loader"}
    if any(runtime["host_tools"].get(role) != WINDOWS_TOOLS[role] for role in native_roles):
        fail("The RAM runner does not select this archive's native Windows tools")
    adapter = runner.load_adapter(root / runner.adapter_path(runtime))
    if platform_name == "sc9820e":
        adapter.validate_runtime(runtime)
        if "initramfs" not in runtime["adapter"]["regions"]:
            fail("Windows archives require a RAM root filesystem")
    elif platform_name == "ums9117":
        adapter.runtime_target_display_name(runtime)
        adapter.adapter_config(runtime["adapter"])
        with (root / runtime["image"]).open("rb") as stream:
            if stream.read(4) != b"DHTB":
                fail("The RAM payload does not have its required header")
    else:
        fail("This platform has no Windows RAM loader")
    ssh = ssh_transport
    try:
        windows = json.loads((root / "windows-host.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"Windows host manifest cannot be read: {error}")
    windows = runner.require_object(windows, {"host_tools", "sha256"}, "Windows host manifest")
    if windows["host_tools"] != tool_paths:
        fail("Windows host tools do not match this archive format")
    hashes = windows["sha256"]
    required_files = set(tool_paths.values())
    if not isinstance(hashes, dict) or not required_files.issubset({*runtime["sha256"], *hashes}):
        fail("Windows host manifest lacks native tool checksums")
    for relative, expected in hashes.items():
        name = runner.relative_name(relative, "Windows file")
        if not isinstance(expected, str) or ssh.SHA256.fullmatch(expected) is None:
            fail(f"Windows file checksum is invalid: {name}")
        runner.require_file(root / name)
        if runner.digest(root / name) != expected:
            fail(f"Windows file checksum differs: {name}")
    return WindowsBundle(
        root,
        runtime,
        {role: root / relative for role, relative in tool_paths.items()},
        adapter,
        ssh,
        generation,
        device_identity,
    )


def exact_usb_devices(snapshot: dict[str, Any], vendor: int, product: int) -> list[str]:
    """Match device roots rather than one interface of a composite device."""
    pattern = re.compile(
        rf"USB\\VID_{vendor:04X}&PID_{product:04X}(?:&REV_[0-9A-F]{{4}})?\\[^\\]+\Z",
        re.IGNORECASE,
    )
    return sorted({item["id"] for item in snapshot["devices"] if pattern.fullmatch(item["id"])})


def bound_interface(snapshot: dict[str, Any], session: dict[str, Any]) -> int | None:
    """Require the exact RNDIS ancestry, MAC and private DHCP endpoint."""
    vendor, product = session["vendor_id"], session["product_id"]
    root_pattern = re.compile(
        rf"USB\\VID_{vendor:04X}&PID_{product:04X}(?:&REV_[0-9A-F]{{4}})?\\"
        rf"{session['usb_serial']}\Z",
        re.IGNORECASE,
    )
    candidates = [
        adapter
        for adapter in snapshot["adapters"]
        if adapter["mac"].replace("-", ":").lower() == session["host_mac"]
        and any(root_pattern.fullmatch(item) for item in adapter["lineage"])
    ]
    if len(candidates) > 1:
        fail("Multiple network adapters match this USB session")
    if not candidates:
        return None
    adapter = candidates[0]
    if adapter["provider"].lower() != "microsoft" or not any(
        re.fullmatch(
            r"USB\\(?:MS_COMP_RNDIS(?:&MS_SUBCOMP_[A-Z0-9]+)?|Class_EF&SubClass_04&Prot_01)",
            value,
            re.IGNORECASE,
        )
        for value in adapter["compatible"]
    ):
        fail("The selected USB network adapter is not using the Windows RNDIS driver")
    index = int(adapter["index"])
    routes = snapshot["routes"]
    if any(route["index"] == index and route["prefix"] == "0.0.0.0/0" for route in routes):
        fail("The phone USB adapter unexpectedly has a default route")
    for route in routes:
        prefix = ipaddress.IPv4Network(route["prefix"], strict=False)
        if (
            prefix.prefixlen >= 30
            and ipaddress.IPv4Address(session["phone_address"]) in prefix
            and route["index"] != index
        ):
            fail("A competing route covers the phone address")
    correct_address = any(
        address["address"] == session["host_address"]
        and address["prefix"] == 30
        and address["state"].lower() == "preferred"
        and address["origin"].lower() == "dhcp"
        for address in adapter["addresses"]
    )
    on_link = any(
        route["index"] == index
        and route["prefix"] == session["network"]
        and route["next_hop"] == "0.0.0.0"  # noqa: S104 -- Windows denotes an on-link route.
        for route in routes
    )
    if (
        adapter["status"].lower() != "up"
        or adapter["dhcp"].lower() != "enabled"
        or not correct_address
        or not on_link
    ):
        return None
    return index


class WindowsHost:
    """Native discovery and private session files without changing host networking."""

    powershell: Path
    icacls: Path
    sid: str

    def __init__(self) -> None:
        """Require the native runtime and built-in read-only discovery commands."""
        if sys.platform != "win32" or platform.machine().lower() not in ("amd64", "x86_64"):
            fail("Use this archive on Windows 10 or 11 x64")
        if sys.getwindowsversion().major < 10:
            fail("Windows 10 or 11 is required")
        if sys.version_info[:2] != (3, 14) or struct.calcsize("P") != 8:
            fail("Use the bundled Python 3.14 x64 runtime")
        system = Path(os.environ["SYSTEMROOT"]) / "System32"
        self.powershell = system / "WindowsPowerShell/v1.0/powershell.exe"
        self.icacls = system / "icacls.exe"
        for path in (self.powershell, self.icacls):
            if not path.is_file():
                fail(f"Required Windows tool is missing: {path.name}")
        self.sid = self.ps("user.ps1")["sid"]
        if not isinstance(self.sid, str) or re.fullmatch(r"S-1-5-[0-9-]+", self.sid) is None:
            fail("Cannot identify the current Windows user")

    def ps(
        self,
        script: str,
        *arguments: str,
        timeout: float = 20,
        cancelled: Callable[[], bool] | None = None,
    ) -> dict[str, Any]:
        """Run one packaged PowerShell file with literal arguments and JSON output."""
        source = Path(__file__).resolve().parents[3] / "common/windows" / script
        if not source.is_file():
            fail(f"Windows command file is missing: {source}")
        argv = [
            str(self.powershell),
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(source),
            *arguments,
        ]
        if cancelled is None:
            result = subprocess.run(
                argv,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=timeout,
                check=False,
                creationflags=_console_creation_flags(),
            )
        else:
            result = _cancellable_discovery(argv, timeout=timeout, cancelled=cancelled)
        if result.returncode:
            fail("Windows discovery failed: " + result.stderr.strip()[-1000:])
        value = json.loads(result.stdout.lstrip("\ufeff"))
        if not isinstance(value, dict):
            fail("Windows discovery returned an unexpected result")
        return value

    def network_snapshot(self, session: dict[str, Any], *, timeout: float = 20) -> dict[str, Any]:
        """Inspect session-MAC adapters and retain the complete IPv4 route table."""
        return self.ps(
            "network.ps1",
            "-HostMac",
            session["host_mac"],
            "-UsbSerial",
            session["usb_serial"],
            "-VendorId",
            str(session["vendor_id"]),
            "-ProductId",
            str(session["product_id"]),
            timeout=timeout,
        )

    def confirm_driver_install(self) -> bool:
        """Require native consent to driver replacement and certificate trust changes."""
        result = self.ps("driver-consent.ps1", timeout=300)
        return result["accepted"] is True

    def install_bootrom_driver(
        self, helper: Path, directory: Path, *, log: Path | None = None
    ) -> None:
        """Elevate before driver preparation and keep the fixed installer output locally."""
        self.require_private(directory)
        if log is None:
            log = directory / "driver.log"
        request = directory / "driver-request.json"
        try:
            with request.open("x", encoding="utf-8") as stream:
                json.dump(
                    {"installer": str(helper), "directory": str(directory), "log": str(log)},
                    stream,
                )
                stream.write("\n")
            self.secure_new(request)
            result = self.ps("driver-elevate.ps1", "-RequestPath", str(request), timeout=300)
        except subprocess.TimeoutExpired:
            fail(
                "USB driver setup exceeded its 300s deadline. Let the Windows installer "
                f"finish, then disconnect the phone and restart Run.cmd; see {log}"
            )
        except SystemExit as error:
            fail(
                f"USB driver setup could not complete: {error}. "
                "Disconnect the phone and restart Run.cmd to retry"
            )
        finally:
            request.unlink(missing_ok=True)
        if result["cancelled"] is True:
            fail("USB driver setup was not authorized; disconnect the phone and restart Run.cmd")
        if result["status"] != 0:
            fail(
                f"USB driver setup failed with status {result['status']}; see {log}. "
                "Disconnect the phone and restart Run.cmd to retry"
            )

    def require_private(self, path: Path) -> None:
        """Reject links and permissions granted outside the account and Windows administrators."""
        if path.is_symlink() or path.is_junction():
            fail(f"Private session path is a link: {path}")
        acl = self.ps("acl.ps1", "-Path", str(path))
        allowed = {self.sid, "S-1-5-18", "S-1-5-32-544", "S-1-3-4"}
        if (
            acl["owner"] != self.sid
            or not acl["protected"]
            or not any(
                rule["sid"] == self.sid and rule["type"] == "Allow" for rule in acl["rules"]
            )
            or any(rule["sid"] not in allowed or rule["type"] != "Allow" for rule in acl["rules"])
        ):
            fail(f"Session path is not private to the current Windows account: {path}")

    def secure_new(self, path: Path, *, directory: bool = False) -> None:
        """Protect a newly created session item before exposing it to a native tool."""
        permission = f"*{self.sid}:" + ("(OI)(CI)F" if directory else "F")
        result = subprocess.run(
            [
                str(self.icacls),
                str(path),
                "/inheritance:r",
                "/grant:r",
                permission,
                "*S-1-5-18:" + ("(OI)(CI)F" if directory else "F"),
                "*S-1-5-32-544:" + ("(OI)(CI)F" if directory else "F"),
                "/remove:g",
                "*S-1-1-0",
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
            creationflags=_console_creation_flags(),
        )
        if result.returncode:
            fail("Cannot protect the local session: " + result.stderr.strip()[-1000:])
        self.require_private(path)

    def runtime_root(self, directory: Path) -> Path:
        """Protect account-owned runtime storage before creating or reusing session files."""
        root = directory
        if root.is_symlink() or root.is_junction():
            fail(f"Private session path is a link: {root}")
        if root.exists():
            if not root.is_dir():
                fail(f"Private session path is not a directory: {root}")
            if self.ps("acl.ps1", "-Path", str(root))["owner"] != self.sid:
                fail(f"Private session directory belongs to another Windows account: {root}")
        else:
            root.mkdir(mode=0o700, parents=True)
        self.secure_new(root, directory=True)
        return root

    def run_tool(  # noqa: PLR0913 -- process inputs and optional USB progress stay explicit.
        self,
        argv: list[str],
        *,
        timeout: int,
        cwd: Path,
        log: Path,
        environment: dict[str, str] | None = None,
        on_usb_ready: Callable[[], None] | None = None,
        on_wait: Callable[[], None] | None = None,
    ) -> None:
        """Run one loader process with a deadline and a retained diagnostic log."""
        child_environment = None
        if environment is not None:
            child_environment = {**os.environ, **environment}
        with log.open("ab") as output:
            offset = output.tell()
            process = subprocess.Popen(
                [argv[0], "--ready-ack", *argv[1:]] if on_usb_ready is not None else argv,
                cwd=cwd,
                stdin=subprocess.PIPE if on_usb_ready is not None else subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
                creationflags=_console_creation_flags(),
                env=child_environment,
            )
            try:
                if on_usb_ready is None:
                    status = process.wait(timeout=timeout)
                else:
                    status = _wait_usb_loader(
                        process,
                        timeout=timeout,
                        log=log,
                        offset=offset,
                        on_usb_ready=on_usb_ready,
                        on_wait=on_wait,
                    )
            except subprocess.TimeoutExpired:
                fail(f"{Path(argv[0]).name} exceeded its {timeout}s deadline; see {log}")
            finally:
                if process.stdin is not None:
                    with suppress(BrokenPipeError):
                        process.stdin.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
        if status:
            fail(f"{Path(argv[0]).name} failed with status {status}; see {log}")


def _write_session(host: WindowsHost, directory: Path, session: dict[str, Any]) -> None:
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=directory, prefix=".session.json.", mode="w", encoding="utf-8", delete=False
        ) as stream:
            temporary = Path(stream.name)
            json.dump(session, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        host.secure_new(temporary)
        temporary.replace(directory / "session.json")
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _prepare_session(
    host: WindowsHost,
    bundle: WindowsBundle,
    *,
    events: Callable[[str], None] | None = None,
    session_root: Path,
    prepared_data_sha256: str | None = None,
) -> dict[str, Any]:
    root = host.runtime_root(session_root)
    if events is not None:
        events("network-preparation")
    occupied = {
        ipaddress.IPv4Network(route["prefix"], strict=False)
        for route in host.ps("routes.ps1")["routes"]
        if ipaddress.IPv4Network(route["prefix"], strict=False).prefixlen > 0
    }
    for path in root.glob("*/session.json"):
        previous = json.loads(path.read_text(encoding="utf-8"))
        occupied.add(ipaddress.IPv4Network(previous["network"]))
    for _attempt in range(4096):
        # The SSH helper owns the shared session addressing and binary ABI.
        network = bundle.ssh._candidate_network(secrets.token_bytes(5))  # noqa: SLF001
        if all(not network.overlaps(used) for used in occupied):
            break
    else:
        fail("Cannot allocate a free private USB network")
    session_id = secrets.token_bytes(32)
    serial = hashlib.sha256(session_id).hexdigest()[:32]
    directory = root / serial
    directory.mkdir()
    try:
        session = _prepare_session_directory(
            host, bundle, directory, network=network, session_id=session_id, events=events
        )
        session["prepared_data_sha256"] = prepared_data_sha256
        _write_session(host, directory, session)
    except BaseException:
        with suppress(OSError):
            shutil.rmtree(directory)
        raise
    else:
        return session


def _prepare_session_directory(  # noqa: PLR0913 -- owned session inputs stay explicit.
    host: WindowsHost,
    bundle: WindowsBundle,
    directory: Path,
    *,
    network: ipaddress.IPv4Network,
    session_id: bytes,
    events: Callable[[str], None] | None,
) -> dict[str, Any]:
    host.secure_new(directory, directory=True)
    serial = directory.name
    macs: list[str] = []
    while len(macs) < 2:
        octets = bytearray(secrets.token_bytes(6))
        octets[0] = (octets[0] & 0xFC) | 0x02
        mac = ":".join(f"{octet:02x}" for octet in octets)
        if mac not in macs:
            macs.append(mac)
    host_mac, device_mac = macs
    private_key = directory / "id_ed25519"
    if events is not None:
        events("session-preparation")
    result = subprocess.run(
        [
            str(bundle.tools["keygen"]),
            "-q",
            "-t",
            "ed25519",
            "-N",
            "",
            "-C",
            "fplinux-ram",
            "-f",
            str(private_key),
        ],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
        creationflags=_console_creation_flags(),
    )
    if result.returncode:
        fail("SSH key generation failed: " + result.stderr.strip()[-1000:])
    host.secure_new(private_key)
    public_key = private_key.with_suffix(".pub")
    host.secure_new(public_key)
    block = bundle.ssh._session_block(  # noqa: SLF001 -- shared binary session ABI.
        session_id,
        secrets.token_bytes(64),
        bundle.ssh._public_key_body(public_key),  # noqa: SLF001 -- shared key format.
        bundle.ssh._usb_config(serial, network, host_mac, device_mac),  # noqa: SLF001
    )
    image = directory / "image.bin"
    if events is not None:
        events("payload-preparation")
    bundle.ssh._copy_personalized_image(  # noqa: SLF001 -- shared image slot verification.
        bundle.root / bundle.runtime["image"], image, bundle.runtime["personalization"], block
    )
    host.secure_new(image)
    linux_usb = bundle.runtime["usb"]["linux_gadget"]
    return {
        "target": bundle.runtime["target"],
        "session_id": session_id.hex(),
        "usb_serial": serial,
        "network": str(network),
        "host_address": str(network.network_address + 1),
        "phone_address": str(network.network_address + 2),
        "host_mac": host_mac,
        "device_mac": device_mac,
        "private_key": str(private_key),
        "known_hosts": str(directory / "known_hosts"),
        "image": str(image),
        "directory": str(directory),
        "vendor_id": linux_usb["vendor_id"],
        "product_id": linux_usb["product_id"],
        "generation": bundle.generation,
        "device_identity": bundle.device_identity,
        "status": "prepared",
        "prepared_data_sha256": None,
    }


class WindowsTransport:
    """The selected archive's SSH client and its exact live USB session."""

    def __init__(self, host: WindowsHost, bundle: WindowsBundle) -> None:
        """Keep Windows effects and archive identity at their respective boundaries."""
        self.host = host
        self.bundle = bundle

    def _ssh_arguments(
        self, session: dict[str, Any], command: str | None = None, *, connect_timeout: int = 5
    ) -> list[str]:
        options = [
            *self.bundle.ssh.ssh_policy_options(
                usb_serial=session["usb_serial"],
                known_hosts=str(session["known_hosts"]).replace("\\", "/"),
                host_address=session["host_address"],
                connect_timeout=connect_timeout,
            ),
            ("HostKeyAlgorithms", "ssh-ed25519"),
            ("GlobalKnownHostsFile", "NUL"),
        ]
        argv = [str(self.bundle.tools["ssh"]), "-4", "-F", "NUL", "-i", session["private_key"]]
        for name, value in options:
            rendered = self.bundle.ssh._ssh_config_value(value)  # noqa: SLF001 -- config quoting.
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
        except _UsbProbeCancelledError:
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
    ) -> subprocess.CompletedProcess[str]:
        """Run dedicated SSH; shared metadata operations leave caller input untouched."""
        self._require_bound(session)
        return subprocess.run(
            self._ssh_arguments(session, command),
            stdin=subprocess.DEVNULL if shared else None,
            capture_output=capture_output,
            text=True,
            check=False,
            creationflags=_console_creation_flags(),
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
                creationflags=_console_creation_flags(),
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
                detail = self.bundle.ssh._stream_stderr_detail(stderr)  # noqa: SLF001
                fail(
                    f"SSH remote command failed with exit status {status}"
                    + (f": {detail}" if detail else "")
                )

    def authenticate(
        self, session: dict[str, Any], *, timeout: float, scan_host_key: bool = True
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
                            str(self.bundle.tools["keyscan"]),
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
                        creationflags=_console_creation_flags(),
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
                    self.bundle.ssh._public_key_body(known_hosts)  # noqa: SLF001 -- key format.
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
                    creationflags=_console_creation_flags(),
                )
            except subprocess.TimeoutExpired:
                continue
            if result.returncode:
                time.sleep(0.5)
                continue
            lines = result.stdout.splitlines()
            if len(lines) != 2 or not hmac.compare_digest(lines[0], session["session_id"]):
                fail("The authenticated phone returned another RAM session identity")
            if not lines[1].endswith(f"-fplinux-{self.bundle.device_identity[:16]}"):
                fail("The authenticated phone returned another kernel build identity")
            try:
                if current_interface() != index:
                    continue
            except subprocess.TimeoutExpired:
                continue
            if session["status"] != "ready":
                session["status"] = "ready"
                _write_session(self.host, Path(session["directory"]), session)
            return
        fail(
            "The exact USB RNDIS and authenticated SSH endpoint "
            "did not become ready before the deadline"
        )

    def _validate_saved_session(self, value: object, directory: Path) -> dict[str, Any]:
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
            value["target"] != self.bundle.runtime["target"]
            or value["generation"] != self.bundle.generation
            or value["device_identity"] != self.bundle.device_identity
            or not isinstance(session_id, str)
            or self.bundle.ssh.SHA256.fullmatch(session_id) is None
            or value["usb_serial"] != hashlib.sha256(bytes.fromhex(session_id)).hexdigest()[:32]
        ):
            fail("Saved phone connection belongs to another archive or RAM session")
        try:
            network = ipaddress.IPv4Network(value["network"], strict=True)
        except TypeError, ValueError:
            fail("Saved phone connection has an invalid network")
        if network.prefixlen != 30 or not self.bundle.ssh._is_rfc1918(network):  # noqa: SLF001
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
        usb = self.bundle.runtime["usb"]["linux_gadget"]
        if (value["vendor_id"], value["product_id"]) != (usb["vendor_id"], usb["product_id"]):
            fail("Saved phone connection has another USB identity")
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

    def _saved_sessions(self, target: str, session_root: Path) -> list[dict[str, Any]]:
        if target != self.bundle.runtime["target"]:
            fail("The requested connection belongs to another target")
        root = self.host.runtime_root(session_root)
        candidates: list[dict[str, Any]] = []
        for state_path in root.glob("*/session.json"):
            try:
                value = json.loads(state_path.read_text(encoding="utf-8"))
            except (OSError, ValueError) as error:
                fail(f"Saved phone connection cannot be read: {error}")
            if isinstance(value, dict) and value.get("status") == "prepared":
                continue
            candidates.append(self._validate_saved_session(value, state_path.parent))
        return candidates

    def load_saved_session(
        self,
        target: str,
        *,
        session_root: Path,
    ) -> dict[str, Any] | None:
        """Read one ready binding without requiring a connected USB device or SSH peer."""
        candidates = self._saved_sessions(target, session_root)
        if not candidates:
            return None
        if len(candidates) > 1:
            fail("Multiple saved phone connections cannot select one RAM session")
        return candidates[0]

    def reacquire_bound_session(
        self, session: dict[str, Any], *, expected_device_identity: str
    ) -> dict[str, Any]:
        """Authenticate a reappearing RAM session against its existing pinned host key."""
        if expected_device_identity != self.bundle.device_identity:
            fail("The requested connection belongs to another kernel build identity")
        self.authenticate(
            session,
            timeout=self.bundle.runtime["usb"]["linux_gadget"]["wait_seconds"],
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
                self._validate_saved_session(value, directory)
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


def _setup_bootrom_driver(
    host: WindowsHost,
    bundle: WindowsBundle,
    directory: Path,
    *,
    events: Callable[[str], None] | None,
    log: Path | None = None,
) -> None:
    """Install after native driver refusal, then await a fresh physical connection."""
    usb = bundle.runtime["usb"]["bootrom"]
    if (usb["vendor_id"], usb["product_id"]) != (0x1782, 0x4D00):
        fail("Automatic USB driver setup is unavailable for this BootROM USB ID")
    try:
        accepted = host.confirm_driver_install()
    except subprocess.TimeoutExpired:
        fail("USB driver consent timed out; disconnect the phone and restart Run.cmd")
    if not accepted:
        fail("USB driver setup was declined; disconnect the phone and restart Run.cmd")
    if events is not None:
        events("driver-setup")
    host.install_bootrom_driver(bundle.tools["driver"], directory, log=log)
    if events is not None:
        events("driver-reconnect")
    deadline = time.monotonic() + usb["wait_seconds"]
    while exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
        if time.monotonic() >= deadline:
            fail("Disconnect the phone after USB driver setup, then restart Run.cmd")
        time.sleep(0.2)


def boot_archive(  # noqa: PLR0913 -- native loading inputs have separate owners.
    bundle: WindowsBundle,
    device_data: Path,
    *,
    events: Callable[[str], None] | None = None,
    log: Path,
    session_root: Path,
    prepared_data_sha256: str | None = None,
    on_wait: Callable[[], None] | None = None,
) -> tuple[WindowsTransport, dict[str, Any]]:
    """Boot one selected archive with its external data capsule and return authenticated SSH."""
    if events is not None:
        events("host-preparation")
    root = bundle.root
    if device_data.is_symlink() or not device_data.is_file():
        fail("The prepared device data capsule is missing")
    host = WindowsHost()
    usb = bundle.runtime["usb"]["bootrom"]
    if exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
        fail(
            "Disconnect the powered-off phone and restart Run.cmd; "
            "connect only after its invitation"
        )
    adapter = bundle.adapter
    config = adapter.adapter_config(bundle.runtime["adapter"])
    log.parent.mkdir(parents=True, exist_ok=True)
    session = _prepare_session(
        host,
        bundle,
        events=events,
        session_root=session_root,
        prepared_data_sha256=prepared_data_sha256,
    )
    directory = Path(session["directory"])
    image = Path(session["image"])
    split_upload = bundle.runtime["identity"]["platform"]["name"] == "sc9820e"
    ready = False
    try:
        loader_directory = directory
        if split_upload:
            loader_directory = directory / "uploads"
            loader_directory.mkdir()
            host.secure_new(loader_directory, directory=True)
            uploads = adapter.prepare_uploads(
                root, bundle.runtime, session, loader_directory, device_data=device_data
            )
            loader_arguments = adapter.loader_arguments(
                bundle.tools["loader"],
                uploads,
                config,
                bus=None,
                device=None,
                wait_seconds=usb["wait_seconds"],
                working_directory=loader_directory,
            )
        else:
            shutil.copyfile(root / bundle.runtime["assets"]["fdl1"], directory / "fdl1.bin")
            loader_arguments = adapter.loader_arguments(
                bundle.tools["loader"],
                Path("fdl1.bin"),
                Path("image.bin"),
                bundle.runtime["addresses"],
                config["exec_distance"],
                wait_seconds=usb["wait_seconds"],
            )
        driver_installed = False

        def usb_ready() -> None:
            if events is not None:
                events("device-found")
                if driver_installed:
                    events("driver-ready")
                events("ram-loader-start")

        while True:
            print(
                f"{bundle.runtime['identity']['target']['display_name']} console RAM boot",
                flush=True,
            )
            print(config["boot_instructions"], flush=True)
            if events is not None:
                events("waiting-for-device")
            try:
                host.run_tool(
                    [loader_arguments[0], "--verbose", "1", *loader_arguments[1:]],
                    timeout=300 + usb["wait_seconds"],
                    cwd=loader_directory,
                    log=log,
                    environment={"LIBUSB_DEBUG": "4"},
                    on_usb_ready=usb_ready,
                    on_wait=on_wait,
                )
            except BootromDriverRequiredError:
                if driver_installed:
                    fail("WinUSB is still unavailable after setup; disconnect and restart Run.cmd")
                _setup_bootrom_driver(host, bundle, directory, events=events, log=log)
                driver_installed = True
            else:
                break
        if events is not None:
            events("ram-loader-complete")
        image.unlink()
        if split_upload:
            shutil.rmtree(loader_directory)
        else:
            (directory / "fdl1.bin").unlink()
            payload = directory / "payload"
            payload.mkdir()
            host.secure_new(payload, directory=True)
            sources = adapter.loader_payload_sources(root, device_data=device_data)
            for name, source in sources.items():
                destination = payload / name
                shutil.copyfile(source, destination)
                host.secure_new(destination)
        print("Waiting for the authenticated Linux connection...", flush=True)
        if events is not None:
            events("linux-start")
        if not split_upload:
            host.run_tool(
                [
                    str(bundle.tools["bridge"]),
                    "--fplinux-handoff",
                    session["session_id"],
                    "--",
                    *adapter.fpdoom_options(config),
                    config["session_name"],
                ],
                timeout=config["handoff_wait_seconds"],
                cwd=payload,
                log=log,
            )
        deadline = time.monotonic() + config["usb_release_wait_seconds"]
        while exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
            if time.monotonic() >= deadline:
                fail("The BootROM USB device did not release after the Linux transition")
            time.sleep(0.2)
        if events is not None:
            events("linux-transition")
        transport = WindowsTransport(host, bundle)
        transport.authenticate(
            session, timeout=bundle.runtime["usb"]["linux_gadget"]["wait_seconds"]
        )
        ready = True
        transport.retire_previous_sessions(session)
        if events is not None:
            events("ssh-ready")
        return transport, session
    finally:
        original_error = sys.exception()
        try:
            for temporary in (image, directory / "fdl1.bin"):
                temporary.unlink(missing_ok=True)
            for stage in (directory / "payload", directory / "uploads", directory / "driver"):
                if stage.exists():
                    shutil.rmtree(stage)
            if not ready:
                shutil.rmtree(directory)
        except OSError:
            if original_error is None:
                raise

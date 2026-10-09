# SPDX-License-Identifier: GPL-2.0-only
"""Load one archive on Windows and bind its RNDIS endpoint to authenticated SSH."""

from __future__ import annotations

import hashlib
import hmac
import importlib.util
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
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import ModuleType
    from typing import BinaryIO

WINDOWS_TOOLS = {
    "loader": "tools/spd_dump.exe",
    "bridge": "tools/libc_server.exe",
    "ssh": "tools/openssh/ssh.exe",
    "keygen": "tools/openssh/ssh-keygen.exe",
    "keyscan": "tools/openssh/ssh-keyscan.exe",
    "driver": "tools/driver/wdi-simple.exe",
}
DRIVER_LICENSE = "licenses/windows-driver/License.rtf"


def _console_creation_flags() -> int:
    return int(getattr(subprocess, "CREATE_NO_WINDOW", 0))


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FPLinux: {message}")


def _load_runner(root: Path) -> ModuleType:
    source = root / "runner/run.py"
    spec = importlib.util.spec_from_file_location("fplinux_ram_runner", source)
    if spec is None or spec.loader is None:
        fail("The archive runner is missing")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


def validate_archive(root: Path) -> WindowsBundle:
    """Check files without discovering USB or creating private host state."""
    runner = _load_runner(root)
    runtime = runner.load_runtime_manifest(root / "runtime-manifest.json")
    for relative, expected in runtime["sha256"].items():
        runner.require_file(root / relative)
        if runner.digest(root / relative) != expected:
            fail(f"Archive file checksum differs: {relative}")
    runtime = runner.validate_runtime_identity(runtime)
    if runtime["transport"] != "usb-ncm":
        fail("This archive does not support the authenticated Linux endpoint")
    adapter = runner.load_adapter(root / runner.ADAPTER_PATH)
    adapter.runtime_target_display_name(runtime)
    adapter.adapter_config(runtime["adapter"])
    ssh = runner.load_module(root / runner.SSH_HELPER_PATH, "ssh_transport")
    identity = ssh.bundle_identity(root, runtime)
    device_identity = ssh.build_manifest_device_identity(root)
    try:
        windows = json.loads((root / "windows-host.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"Windows host manifest cannot be read: {error}")
    windows = runner.require_object(windows, {"host_tools", "sha256"}, "Windows host manifest")
    if windows["host_tools"] != WINDOWS_TOOLS:
        fail("Windows host tools do not match this archive format")
    hashes = windows["sha256"]
    required_files = {*WINDOWS_TOOLS.values(), DRIVER_LICENSE}
    if not isinstance(hashes, dict) or not required_files.issubset(hashes):
        fail("Windows host manifest lacks native tool or driver license checksums")
    for relative, expected in hashes.items():
        name = runner.relative_name(relative, "Windows file")
        if not isinstance(expected, str) or ssh.SHA256.fullmatch(expected) is None:
            fail(f"Windows file checksum is invalid: {name}")
        runner.require_file(root / name)
        if runner.digest(root / name) != expected:
            fail(f"Windows file checksum differs: {name}")
    if (root / runtime["image"]).read_bytes()[:4] != b"DHTB":
        fail("The RAM payload does not have its required header")
    return WindowsBundle(
        root,
        runtime,
        {role: root / relative for role, relative in WINDOWS_TOOLS.items()},
        adapter,
        ssh,
        identity["bundle_generation"],
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

    def ps(self, script: str, *arguments: str, timeout: int = 20) -> dict[str, Any]:
        """Run one packaged PowerShell file with literal arguments and JSON output."""
        source = Path(__file__).resolve().parent / "windows" / script
        if not source.is_file():
            fail(f"Windows command file is missing: {source}")
        result = subprocess.run(
            [
                str(self.powershell),
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(source),
                *arguments,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=timeout,
            check=False,
            creationflags=_console_creation_flags(),
        )
        if result.returncode:
            fail("Windows discovery failed: " + result.stderr.strip()[-1000:])
        value = json.loads(result.stdout.lstrip("\ufeff"))
        if not isinstance(value, dict):
            fail("Windows discovery returned an unexpected result")
        return value

    def network_snapshot(self, session: dict[str, Any]) -> dict[str, Any]:
        """Inspect session-MAC adapters and retain the complete IPv4 route table."""
        return self.ps("network.ps1", "-HostMac", session["host_mac"])

    def confirm_driver_install(self, license_path: Path) -> bool:
        """Require native consent and explicit acceptance of the bundled Microsoft terms."""
        result = self.ps("driver-consent.ps1", "-LicensePath", str(license_path), timeout=300)
        return result["accepted"] is True

    def install_bootrom_driver(self, helper: Path, directory: Path) -> None:
        """Elevate before driver preparation and keep the fixed installer output locally."""
        self.require_private(directory)
        log = directory / "driver.log"
        request = directory / "driver-request.json"
        try:
            with request.open("x", encoding="utf-8") as stream:
                json.dump({"installer": str(helper), "directory": str(directory)}, stream)
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

    def runtime_root(self, archive: Path) -> Path:
        """Open private session storage beside this archive's runtime cache."""
        root = archive / ".cache/sessions"
        if not root.exists():
            root.mkdir(parents=True)
            self.secure_new(root, directory=True)
        else:
            self.require_private(root)
        return root

    def run_tool(
        self,
        argv: list[str],
        *,
        timeout: int,
        cwd: Path,
        log: Path,
        environment: dict[str, str] | None = None,
    ) -> None:
        """Run one loader process with a deadline and a retained diagnostic log."""
        child_environment = None
        if environment is not None:
            child_environment = {**os.environ, **environment}
        with log.open("ab") as output:
            process = subprocess.Popen(
                argv,
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
                creationflags=_console_creation_flags(),
                env=child_environment,
            )
            try:
                status = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                fail(f"{Path(argv[0]).name} exceeded its {timeout}s deadline; see {log}")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        if status:
            fail(f"{Path(argv[0]).name} failed with status {status}; see {log}")


def _write_session(host: WindowsHost, directory: Path, session: dict[str, Any]) -> None:
    temporary = directory / "session.json.tmp"
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(session, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        host.secure_new(temporary)
        temporary.replace(directory / "session.json")
    finally:
        temporary.unlink(missing_ok=True)


def _prepare_session(
    host: WindowsHost,
    bundle: WindowsBundle,
    *,
    events: Callable[[str], None] | None = None,
) -> dict[str, Any]:
    root = host.runtime_root(bundle.root)
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
    host.secure_new(directory, directory=True)
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
    session = {
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
    }
    _write_session(host, directory, session)
    return session


class WindowsTransport:
    """The selected archive's SSH client and its exact live USB session."""

    def __init__(self, host: WindowsHost, bundle: WindowsBundle) -> None:
        """Keep Windows effects and archive identity at their respective boundaries."""
        self.host = host
        self.bundle = bundle

    def _ssh_arguments(
        self, session: dict[str, Any], command: str, *, connect_timeout: int = 5
    ) -> list[str]:
        options = {
            "BatchMode": "yes",
            "IdentitiesOnly": "yes",
            "IdentityAgent": "none",
            "PasswordAuthentication": "no",
            "KbdInteractiveAuthentication": "no",
            "PreferredAuthentications": "publickey",
            "NumberOfPasswordPrompts": "0",
            "StrictHostKeyChecking": "yes",
            "HostKeyAlgorithms": "ssh-ed25519",
            "UserKnownHostsFile": str(session["known_hosts"]).replace("\\", "/"),
            "GlobalKnownHostsFile": "NUL",
            "HostKeyAlias": f"fplinux-{session['usb_serial']}",
            "CheckHostIP": "no",
            "ClearAllForwardings": "yes",
            "ForwardAgent": "no",
            "ForwardX11": "no",
            "Tunnel": "no",
            "PermitLocalCommand": "no",
            "ProxyCommand": "none",
            "ProxyJump": "none",
            "BindAddress": session["host_address"],
            "ConnectTimeout": str(connect_timeout),
            "ConnectionAttempts": "1",
            "ServerAliveInterval": "5",
            "ServerAliveCountMax": "3",
            "LogLevel": "ERROR",
        }
        argv = [str(self.bundle.tools["ssh"]), "-4", "-F", "NUL", "-i", session["private_key"]]
        for name, value in options.items():
            rendered = self.bundle.ssh._ssh_config_value(value)  # noqa: SLF001 -- config quoting.
            if name in {"IdentityAgent", "ProxyCommand", "ProxyJump"}:
                rendered = "none"
            argv.extend(["-o", f"{name}={rendered}"])
        return [*argv, f"root@{session['phone_address']}", command]

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

    def run_remote(
        self,
        session: dict[str, Any],
        command: str,
        *,
        capture_output: bool = False,
        shared: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        """Run one command through a dedicated authenticated transport."""
        del shared
        self._require_bound(session)
        return subprocess.run(
            self._ssh_arguments(session, command),
            capture_output=capture_output,
            text=True,
            check=False,
            creationflags=_console_creation_flags(),
        )

    def stream_remote(
        self,
        session: dict[str, Any],
        command: str,
        destination: BinaryIO,
        *,
        timeout: float,
    ) -> None:
        """Keep binary stdout separate from diagnostics and terminate a timed-out reader."""
        if timeout <= 0:
            message = "SSH stream timeout must be positive"
            raise ValueError(message)
        self._require_bound(session)
        with tempfile.TemporaryFile(mode="w+b") as stderr:
            process = subprocess.Popen(
                self._ssh_arguments(session, command),
                stdin=subprocess.DEVNULL,
                stdout=destination,
                stderr=stderr,
                creationflags=_console_creation_flags(),
            )
            try:
                try:
                    status = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    fail(f"SSH remote command timed out after {timeout:g}s")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            if status:
                detail = self.bundle.ssh._stream_stderr_detail(stderr)  # noqa: SLF001
                fail(
                    f"SSH remote command failed with exit status {status}"
                    + (f": {detail}" if detail else "")
                )

    def authenticate(self, session: dict[str, Any], *, timeout: int) -> None:
        """Pin a key only after USB binding, then verify RAM and kernel identities."""
        deadline = time.monotonic() + timeout
        pinned = False
        while time.monotonic() < deadline:
            index = bound_interface(self.host.network_snapshot(session), session)
            if index is None:
                time.sleep(0.5)
                continue
            try:
                if not pinned:
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
                        timeout=5,
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
                if bound_interface(self.host.network_snapshot(session), session) != index:
                    continue
                remaining = max(1, min(5, int(deadline - time.monotonic())))
                result = subprocess.run(
                    self._ssh_arguments(
                        session, "fplinux-session-id; uname -r", connect_timeout=remaining
                    ),
                    capture_output=True,
                    text=True,
                    timeout=remaining + 1,
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
            if bound_interface(self.host.network_snapshot(session), session) != index:
                continue
            session["status"] = "ready"
            _write_session(self.host, Path(session["directory"]), session)
            return
        fail(
            "The exact USB RNDIS and authenticated SSH endpoint "
            "did not become ready before the deadline"
        )


def _wait_bootrom(host: WindowsHost, usb: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    deadline = time.monotonic() + usb["wait_seconds"]
    while time.monotonic() < deadline:
        snapshot = host.ps("devices.ps1")
        devices = exact_usb_devices(snapshot, usb["vendor_id"], usb["product_id"])
        if len(devices) > 1:
            fail("Multiple BootROM phones are connected; keep only the selected phone connected")
        if devices:
            return devices[0], snapshot
        time.sleep(0.2)
    fail("The selected phone did not appear in BootROM mode before the deadline")


def _bootrom_uses_winusb(snapshot: dict[str, Any], instance: str) -> bool:
    matches = [item for item in snapshot["devices"] if item["id"].lower() == instance.lower()]
    if len(matches) != 1:
        fail("The selected BootROM phone is no longer present")
    return str(matches[0].get("service", "")).lower() == "winusb"


def _ensure_bootrom_driver(  # noqa: PLR0913 -- selected snapshot and driver setup stay explicit.
    host: WindowsHost,
    bundle: WindowsBundle,
    instance: str,
    directory: Path,
    *,
    snapshot: dict[str, Any],
    events: Callable[[str], None] | None,
) -> None:
    if _bootrom_uses_winusb(snapshot, instance):
        return
    usb = bundle.runtime["usb"]["bootrom"]
    if (usb["vendor_id"], usb["product_id"]) != (0x1782, 0x4D00):
        fail("Automatic USB driver setup is unavailable for this BootROM USB ID")
    try:
        accepted = host.confirm_driver_install(bundle.root / DRIVER_LICENSE)
    except subprocess.TimeoutExpired:
        fail("USB driver consent timed out; disconnect the phone and restart Run.cmd")
    if not accepted:
        fail("USB driver setup was declined; disconnect the phone and restart Run.cmd")
    if events is not None:
        events("driver-setup")
    host.install_bootrom_driver(bundle.tools["driver"], directory)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        snapshot = host.ps("devices.ps1")
        present = exact_usb_devices(snapshot, usb["vendor_id"], usb["product_id"])
        if len(present) > 1:
            fail("Multiple BootROM phones are connected; keep only the selected phone connected")
        if instance in present and _bootrom_uses_winusb(snapshot, instance):
            if events is not None:
                events("driver-ready")
            return
        time.sleep(0.2)
    fail(
        "WinUSB did not become ready for the selected BootROM phone after setup. "
        "Disconnect the phone and restart Run.cmd to retry"
    )


def boot_archive(
    root: Path,
    device_data: Path,
    *,
    events: Callable[[str], None] | None = None,
) -> tuple[WindowsTransport, dict[str, Any]]:
    """Boot one selected archive with its external data capsule and return authenticated SSH."""
    if events is not None:
        events("host-preparation")
    bundle = validate_archive(root)
    if device_data.is_symlink() or not device_data.is_file():
        fail("The prepared device data capsule is missing")
    host = WindowsHost()
    usb = bundle.runtime["usb"]["bootrom"]
    if exact_usb_devices(host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]):
        fail(
            "Disconnect the powered-off phone and restart Run.cmd; "
            "connect only after its invitation"
        )
    session = _prepare_session(host, bundle, events=events)
    directory = Path(session["directory"])
    image = Path(session["image"])
    adapter = bundle.adapter
    config = adapter.adapter_config(bundle.runtime["adapter"])
    try:
        print(adapter.runtime_target_display_name(bundle.runtime), flush=True)
        print(config["boot_instructions"], flush=True)
        if events is not None:
            events("waiting-for-device")
        bootrom, snapshot = _wait_bootrom(host, usb)
        if events is not None:
            events("device-found")
        _ensure_bootrom_driver(host, bundle, bootrom, directory, snapshot=snapshot, events=events)
        fdl = directory / "fdl1.bin"
        shutil.copyfile(root / bundle.runtime["assets"]["fdl1"], fdl)
        print("Loading the phone into RAM...", flush=True)
        if events is not None:
            events("ram-loader-start")
        loader_arguments = adapter.loader_arguments(
            bundle.tools["loader"],
            Path("fdl1.bin"),
            Path("image.bin"),
            bundle.runtime["addresses"],
            config["exec_distance"],
        )
        host.run_tool(
            [loader_arguments[0], "--verbose", "1", *loader_arguments[1:]],
            timeout=300,
            cwd=directory,
            log=root / ".cache/run.log",
            environment={"LIBUSB_DEBUG": "4"},
        )
        if events is not None:
            events("ram-loader-complete")
        fdl.unlink()
        image.unlink()
        maps = directory / "maps"
        maps.mkdir()
        host.secure_new(maps, directory=True)
        shutil.copyfile(device_data, maps / "device-data.cpio")
        host.secure_new(maps / "device-data.cpio")
        for role in ("pinmap", "keymap"):
            extracted = device_data.parent / f"{role}.bin"
            canonical = bundle.runtime["assets"].get(role)
            if extracted.is_file():
                shutil.copyfile(extracted, maps / f"{role}.bin")
            elif canonical is not None:
                shutil.copyfile(root / canonical, maps / f"{role}.bin")
            elif role == "pinmap":
                (maps / "pinmap.bin").write_bytes(adapter.NEUTRAL_PINMAP)
            mapped = maps / f"{role}.bin"
            if mapped.is_file():
                host.secure_new(mapped)
        print("Waiting for the authenticated Linux connection...", flush=True)
        if events is not None:
            events("linux-start")
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
            cwd=maps,
            log=root / ".cache/run.log",
        )
        deadline = time.monotonic() + config["usb_release_wait_seconds"]
        while bootrom in exact_usb_devices(
            host.ps("devices.ps1"), usb["vendor_id"], usb["product_id"]
        ):
            if time.monotonic() >= deadline:
                fail("The BootROM USB device did not release after the Linux transition")
            time.sleep(0.2)
        if events is not None:
            events("linux-transition")
        transport = WindowsTransport(host, bundle)
        transport.authenticate(
            session, timeout=bundle.runtime["usb"]["linux_gadget"]["wait_seconds"]
        )
        if events is not None:
            events("ssh-ready")
        return transport, session
    finally:
        image.unlink(missing_ok=True)

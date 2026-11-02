# SPDX-License-Identifier: GPL-2.0-only
"""Native Windows process and device access."""

from __future__ import annotations

import ipaddress
import json
import os
import platform
import re
import struct
import subprocess
import sys
import time
from contextlib import suppress
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from collections.abc import Callable

_USB_READY_MARKERS = (b"FPLINUX_BOOTROM_READY\n", b"FPLINUX_BOOTROM_READY\r\n")


def console_creation_flags() -> int:
    return int(getattr(subprocess, "CREATE_NO_WINDOW", 0))


def fail(message: str) -> NoReturn:
    raise SystemExit(f"FPLinux: {message}")


class BootromDriverRequiredError(Exception):
    """The native loader could not open or claim an unsupported USB driver."""


class UsbProbeCancelledError(Exception):
    """The owner stopped a read-only Windows device probe."""


def _cancellable_discovery(
    argv: list[str], *, environment: dict[str, str], timeout: float, cancelled: Callable[[], bool]
) -> subprocess.CompletedProcess[str]:
    """Stop and reap discovery when its USB observer no longer owns the result."""
    if cancelled():
        raise UsbProbeCancelledError
    process = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        env=environment,
        creationflags=console_creation_flags(),
    )
    deadline = time.monotonic() + timeout
    try:
        while True:
            if cancelled():
                raise UsbProbeCancelledError
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
        source = Path(__file__).resolve().parents[4] / "common/windows" / script
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
        environment = {
            name: value for name, value in os.environ.items() if name.upper() != "PSMODULEPATH"
        }
        if cancelled is None:
            result = subprocess.run(
                argv,
                capture_output=True,
                text=True,
                encoding="utf-8",
                env=environment,
                timeout=timeout,
                check=False,
                creationflags=console_creation_flags(),
            )
        else:
            result = _cancellable_discovery(
                argv, environment=environment, timeout=timeout, cancelled=cancelled
            )
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
            creationflags=console_creation_flags(),
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
                creationflags=console_creation_flags(),
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

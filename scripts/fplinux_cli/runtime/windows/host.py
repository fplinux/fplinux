# SPDX-License-Identifier: GPL-2.0-only
"""Native Windows process and device access."""

from __future__ import annotations

import ipaddress
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import suppress
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

if TYPE_CHECKING:
    from collections.abc import Callable
    from concurrent.futures import Future
    from typing import BinaryIO

_USB_READY_RECORDS = (b"FPLINUX_BOOTROM_READY\n", b"FPLINUX_BOOTROM_READY\r\n")
_USB_READY_READ_BYTES = max(map(len, _USB_READY_RECORDS)) + 1


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


def _loader_status_before_ack(process: subprocess.Popen[bytes], *, deadline: float) -> int:
    status = process.wait(timeout=max(0, deadline - time.monotonic()))
    if status == 2:
        raise BootromDriverRequiredError
    if status == 0:
        fail("The RAM loader exited without confirming its USB connection")
    return status


def _wait_usb_loader(  # noqa: PLR0913 -- native progress and reconnect observers stay explicit.
    process: subprocess.Popen[bytes],
    readiness: Future[bytes],
    *,
    timeout: int,
    output: BinaryIO,
    on_usb_ready: Callable[[], None],
    on_wait: Callable[[], None] | None,
) -> int:
    """Observe this child's USB acquisition without delaying its native discovery."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(process.args, timeout)
        try:
            record = readiness.result(timeout=min(0.1, remaining))
        except TimeoutError:
            if on_wait is not None and not readiness.done():
                on_wait()
        else:
            break

    if not record:
        return _loader_status_before_ack(process, deadline=deadline)
    if record not in _USB_READY_RECORDS:
        fail("The RAM loader returned an invalid USB readiness response")
    if process.stdin is None:
        fail("USB loader acknowledgement pipe is unavailable")
    output.write(record)
    output.flush()
    on_usb_ready()
    if time.monotonic() >= deadline:
        raise subprocess.TimeoutExpired(process.args, timeout)
    try:
        process.stdin.write(b"\x06")
        process.stdin.flush()
    except BrokenPipeError:
        return _loader_status_before_ack(process, deadline=deadline)
    return process.wait(timeout=max(0, deadline - time.monotonic()))


def exact_usb_devices(snapshot: dict[str, Any], vendor: int, product: int) -> list[str]:
    """Match device roots rather than one interface of a composite device."""
    pattern = re.compile(
        rf"USB\\VID_{vendor:04X}&PID_{product:04X}(?:&REV_[0-9A-F]{{4}})?\\[^\\]+\Z",
        re.IGNORECASE,
    )
    return sorted({item["id"] for item in snapshot["devices"] if pattern.fullmatch(item["id"])})


def bound_interface(
    snapshot: dict[str, Any],
    session: dict[str, Any],
    *,
    on_rndis: Callable[[], None] | None = None,
) -> int | None:
    """Require the exact RNDIS ancestry, MAC and private DHCP endpoint."""
    vendor, product = session["vendor_id"], session["product_id"]
    root_pattern = re.compile(
        rf"USB\\VID_{vendor:04X}&PID_{product:04X}(?:&REV_[0-9A-F]{{4}})?\\"
        rf"{session['usb_serial']}\Z",
        re.IGNORECASE,
    )
    host_mac = None if session.get("card_access") else session["host_mac"]
    candidates = [
        adapter
        for adapter in snapshot["adapters"]
        if (host_mac is None or adapter["mac"].replace("-", ":").lower() == host_mac)
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
    if on_rndis is not None:
        on_rndis()
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
        if sys.version_info[:2] != (3, 15) or struct.calcsize("P") != 8:
            fail("Use the bundled Python 3.15 x64 runtime")
        system = Path(os.environ["SYSTEMROOT"]) / "System32"
        self.powershell = system / "WindowsPowerShell/v1.0/powershell.exe"
        self.icacls = system / "icacls.exe"
        for path in (self.powershell, self.icacls):
            if not path.is_file():
                fail(f"Required Windows tool is missing: {path.name}")
        self.sid = self.ps("user.ps1")["sid"]
        if not isinstance(self.sid, str) or re.fullmatch(r"S-1-5-[0-9-]+", self.sid) is None:
            fail("Cannot identify the current Windows user")

    def route_networks(self) -> set[ipaddress.IPv4Network]:
        """Return occupied IPv4 ranges without counting the default route."""
        routes = {
            ipaddress.IPv4Network(route["prefix"], strict=False)
            for route in self.ps("routes.ps1")["routes"]
        }
        return {route for route in routes if route.prefixlen > 0}

    def ps(
        self,
        script: str,
        *arguments: str,
        timeout: float = 20,
        cancelled: Callable[[], bool] | None = None,
    ) -> dict[str, Any]:
        """Run one packaged PowerShell file with literal arguments and JSON output."""
        module = Path(__file__).resolve()
        root = module.parents[4]
        # Both delivered launchers use the root entry point and keep their PowerShell paths.
        if (root / "fplinux.py").is_file():
            scripts = root / "common/windows"
        else:
            scripts = module.with_name("powershell")
        source = scripts / script
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
        """Inspect the bound USB adapters and retain the complete IPv4 route table."""
        host_mac = None if session.get("card_access") else session["host_mac"]
        return self.ps(
            "network.ps1",
            *(("-HostMac", host_mac) if host_mac is not None else ()),
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

    def bootrom_driver_missing(
        self, vendor: int, product: int, *, cancelled: Callable[[], bool] | None = None
    ) -> bool:
        """Recognize a missing driver only on the selected physical USB root."""
        snapshot = self.ps(
            "devices.ps1",
            "-DriverInfo",
            "-VendorId",
            str(vendor),
            "-ProductId",
            str(product),
            cancelled=cancelled,
        )
        devices = set(exact_usb_devices(snapshot, vendor, product))
        return any(
            device["id"] in devices and device.get("problem_code") == 28
            for device in snapshot["devices"]
        )

    def install_bootrom_driver(
        self, helper: Path, directory: Path, *, log: Path | None = None
    ) -> None:
        """Prepare the driver in private temporary storage, independent of the archive path."""
        self.require_private(directory)
        if log is None:
            log = directory / "driver.log"
        workspace = Path(tempfile.mkdtemp(prefix="fplinux-driver-")).resolve()
        request = workspace / "driver-request.json"
        invoked = False
        completed = False
        try:
            self.secure_new(workspace, directory=True)
            destination = workspace / "driver"
            # The native helper has separate UTF-16 directory and UTF-8 file-path limits.
            directory_units = len(str(destination).encode("utf-16-le")) // 2
            inf_bytes = len(str(destination / "fplinux-bootrom.inf").encode("utf-8"))
            if directory_units >= 248 or inf_bytes >= 260:
                fail("The Windows temporary path is too long for USB driver preparation")
            with request.open("x", encoding="utf-8") as stream:
                json.dump(
                    {
                        "installer": str(helper.resolve()),
                        "directory": str(workspace),
                        "driver_directory": str(destination),
                        "log": str(log.resolve()),
                    },
                    stream,
                )
                stream.write("\n")
            self.secure_new(request)
            with log.open("a", encoding="utf-8") as stream:
                stream.write(f"Driver setup workspace: {workspace}\n")
            invoked = True
            try:
                result = self.ps("driver-elevate.ps1", "-RequestPath", str(request), timeout=300)
            except subprocess.TimeoutExpired:
                fail(
                    "USB driver setup exceeded its 300s deadline. Let the Windows installer "
                    f"finish, then disconnect the phone and restart Run.cmd; see {log}. "
                    f"Driver files are retained in {workspace}"
                )
            except SystemExit as error:
                fail(
                    f"USB driver setup could not complete: {error}. "
                    f"Driver files are retained in {workspace}. "
                    "Disconnect the phone and restart Run.cmd to retry"
                )
            completed = True
            if result["cancelled"] is True:
                fail(
                    "USB driver setup was not authorized; disconnect the phone and restart Run.cmd"
                )
            if result["status"] != 0:
                fail(
                    f"USB driver setup failed with status {result['status']}; see {log}. "
                    "Disconnect the phone and restart Run.cmd to retry"
                )
        finally:
            if not invoked or completed:
                original_error = sys.exception()
                try:
                    shutil.rmtree(workspace)
                except OSError:
                    if original_error is None:
                        raise

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
        with (
            log.open("ab") as output,
            subprocess.Popen(
                [argv[0], "--ready-ack", *argv[1:]] if on_usb_ready is not None else argv,
                cwd=cwd,
                stdin=subprocess.PIPE if on_usb_ready is not None else subprocess.DEVNULL,
                stdout=subprocess.PIPE if on_usb_ready is not None else output,
                stderr=output if on_usb_ready is not None else subprocess.STDOUT,
                creationflags=console_creation_flags(),
                env=child_environment,
            ) as process,
            ThreadPoolExecutor(max_workers=1, thread_name_prefix="fplinux-loader") as reader,
        ):
            try:
                if on_usb_ready is None:
                    status = process.wait(timeout=timeout)
                else:
                    assert process.stdout is not None
                    readiness = reader.submit(process.stdout.readline, _USB_READY_READ_BYTES)
                    status = _wait_usb_loader(
                        process,
                        readiness,
                        timeout=timeout,
                        output=output,
                        on_usb_ready=on_usb_ready,
                        on_wait=on_wait,
                    )
            except subprocess.TimeoutExpired:
                fail(f"{Path(argv[0]).name} exceeded its {timeout}s deadline; see {log}")
            finally:
                # Termination closes the native writer before the reading task joins.
                if process.poll() is None:
                    process.kill()
                process.wait()
                if process.stdin is not None:
                    with suppress(BrokenPipeError):
                        process.stdin.close()
        if status:
            fail(f"{Path(argv[0]).name} failed with status {status}; see {log}")

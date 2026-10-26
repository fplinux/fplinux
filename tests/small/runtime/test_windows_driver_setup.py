# SPDX-License-Identifier: GPL-2.0-only
"""Test driver decisions with controlled Windows consent, UAC and device boundaries."""

from __future__ import annotations

import json
import subprocess
from typing import TYPE_CHECKING, Any, override

import pytest
from fplinux_cli.runtime import windows_host

from tests.windows_host_support import StubWindowsHost, admitted_windows, archive_files

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


class DriverHost(StubWindowsHost):
    """Replace native driver effects without executing an installer or accessing USB."""

    def __init__(
        self,
        *,
        consent: bool = True,
        setup_error: str | None = None,
        disconnects: bool = True,
    ) -> None:
        """Keep one selected synthetic BootROM and explicit user/setup outcomes."""
        super().__init__({"devices": [{"id": "USB\\VID_1782&PID_4D00\\selected"}]})
        self.consent = consent
        self.setup_error = setup_error
        self.disconnects = disconnects
        self.effects: list[tuple[str, Any]] = []

    def confirm_driver_install(self) -> bool:
        """Model the native dialog result; this does not establish real GUI behavior."""
        self.effects.append(("consent", None))
        return self.consent

    def install_bootrom_driver(
        self, helper: Path, directory: Path, *, log: Path | None = None
    ) -> None:
        """Model a completed elevated setup or a cancelled/failed native boundary."""
        del log
        self.effects.append(("installer", (helper, directory)))
        if self.setup_error is not None:
            windows_host.fail(self.setup_error)
        if self.disconnects:
            self.snapshot["devices"] = []


def ensure_driver(host: DriverHost, root: Path, events: list[str]) -> None:
    """Exercise the production decision with a real validated temporary archive."""
    bundle = admitted_windows(archive_files(root))
    directory = root / "private-session"
    directory.mkdir()
    windows_host._setup_bootrom_driver(  # noqa: SLF001 -- selected host component boundary.
        host,
        bundle,
        directory,
        events=events.append,
    )


def test_declined_system_changes_stop_before_any_installer(tmp_path: Path) -> None:
    host = DriverHost(consent=False)
    events: list[str] = []
    with pytest.raises(SystemExit, match=r"setup was declined.*restart Run[.]cmd"):
        ensure_driver(host, tmp_path, events)
    assert host.effects == [("consent", None)]
    assert events == []


def test_accepted_setup_waits_for_disconnect_without_claiming_usb_ready(tmp_path: Path) -> None:
    host = DriverHost()
    events: list[str] = []
    ensure_driver(host, tmp_path, events)
    assert host.effects == [
        ("consent", None),
        ("installer", (tmp_path / "tools/driver/wdi-simple.exe", tmp_path / "private-session")),
    ]
    assert host.snapshot["devices"] == []
    assert events == ["driver-setup", "driver-reconnect"]


@pytest.mark.parametrize("error", ["setup was not authorized", "setup failed with status 7"])
def test_cancelled_or_failed_setup_never_reports_ready(tmp_path: Path, error: str) -> None:
    host = DriverHost(setup_error=error)
    events: list[str] = []
    with pytest.raises(SystemExit, match=error):
        ensure_driver(host, tmp_path, events)
    assert events == ["driver-setup"]
    assert len([effect for effect in host.effects if effect[0] == "installer"]) == 1


def test_installed_driver_cannot_start_next_attempt_before_disconnect(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    host = DriverHost(disconnects=False)
    now = 0.0

    def advance_clock(delay: float) -> None:
        nonlocal now
        now += delay

    monkeypatch.setattr("fplinux_cli.runtime.windows_host.time.monotonic", lambda: now)
    monkeypatch.setattr("fplinux_cli.runtime.windows_host.time.sleep", advance_clock)
    events: list[str] = []
    with pytest.raises(SystemExit, match=r"Disconnect the phone after USB driver setup"):
        ensure_driver(host, tmp_path, events)
    assert events == ["driver-setup", "driver-reconnect"]
    assert now >= 30


@pytest.mark.parametrize(
    ("result", "error"),
    [
        ({"cancelled": False, "status": 0}, None),
        ({"cancelled": True}, "setup was not authorized"),
        ({"cancelled": False, "status": 7}, "setup failed with status 7"),
    ],
)
def test_driver_request_preserves_literal_paths_and_cleans_up_after_result(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    result: dict[str, Any],
    error: str | None,
) -> None:
    host = StubWindowsHost({})
    host.powershell = tmp_path / "WindowsPowerShell/powershell.exe"
    directory = tmp_path / "Private ' USB $(no-command) Юникод"
    directory.mkdir()
    helper = tmp_path / "Archive ' USB $(no-command) Юникод/tools/driver/wdi-simple.exe"
    requests: list[tuple[str, tuple[str, ...], int]] = []
    request_paths: list[Path] = []
    secured: list[Path] = []

    def secure_request(path: Path, *, directory: bool = False) -> None:
        del directory
        assert path.exists()
        secured.append(path)

    def native_result(script: str, *arguments: str, timeout: int = 20) -> dict[str, Any]:
        requests.append((script, arguments, timeout))
        assert arguments[0] == "-RequestPath"
        request = directory / "driver-request.json"
        assert arguments[1] == str(request)
        assert request in secured
        assert json.loads(request.read_text(encoding="utf-8")) == {
            "installer": str(helper),
            "directory": str(directory),
            "log": str(directory / "driver.log"),
        }
        request_paths.append(request)
        return result

    monkeypatch.setattr(host, "secure_new", secure_request)
    monkeypatch.setattr(host, "ps", native_result)
    if error is None:
        host.install_bootrom_driver(helper, directory)
    else:
        with pytest.raises(SystemExit, match=error):
            host.install_bootrom_driver(helper, directory)
    assert requests == [
        ("driver-elevate.ps1", ("-RequestPath", str(directory / "driver-request.json")), 300)
    ]
    assert request_paths
    assert all(not request.exists() for request in request_paths)


def test_uac_deadline_reports_retry_without_starting_another_installer(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    host = StubWindowsHost({})
    host.powershell = tmp_path / "powershell.exe"

    requests: list[Path] = []

    def expired(script: str, *arguments: str, timeout: int = 20) -> dict[str, Any]:
        request = tmp_path / "driver-request.json"
        assert arguments == ("-RequestPath", str(request))
        assert request.exists()
        requests.append(request)
        raise subprocess.TimeoutExpired(script, timeout)

    monkeypatch.setattr(host, "ps", expired)
    with pytest.raises(SystemExit, match=r"Let the Windows installer finish.*restart Run[.]cmd"):
        host.install_bootrom_driver(tmp_path / "helper.exe", tmp_path)
    assert requests == [tmp_path / "driver-request.json"]
    assert not requests[0].exists()


def test_driver_helper_checksum_is_required_before_windows_effects(tmp_path: Path) -> None:
    root = archive_files(tmp_path)
    (root / "tools/driver/wdi-simple.exe").write_bytes(b"replaced helper")
    with pytest.raises(SystemExit, match="Windows file checksum differs: tools/driver"):
        admitted_windows(root)


@pytest.mark.parametrize(
    "mode", ["ready", "setup", "declined", "still-unsupported", "transfer-failed"]
)
def test_native_usb_acquisition_controls_progress_and_driver_setup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, mode: str
) -> None:
    root = archive_files(tmp_path)
    capsule = tmp_path / "device-data.cpio"
    capsule.write_bytes(b"controlled external device capsule")
    directory = tmp_path / "private-session"
    directory.mkdir()
    image = directory / "image.bin"
    image.write_bytes(b"synthetic prepared image")
    events: list[str] = []

    class LoaderReachedError(Exception):
        """Stop after the loader boundary; no real Windows process is run."""

    class BootHost(DriverHost):
        """Model native acquisition and fail if PnP delays the first attempt."""

        attempts = 0

        def ps(
            self,
            script: str,
            *arguments: str,
            timeout: float = 20,
            cancelled: Callable[[], bool] | None = None,
        ) -> dict[str, Any]:
            del script, arguments, timeout, cancelled
            assert "waiting-for-device" not in events or "driver-reconnect" in events
            return self.snapshot if "driver-reconnect" in events else {"devices": []}

        @override
        def run_tool(
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
            del cwd, log, on_wait
            self.attempts += 1
            self.effects.append(("loader", argv[0]))
            assert events[-1] == "waiting-for-device"
            assert argv[argv.index("--wait") + 1] == "30"
            assert timeout == 330
            assert argv[1:3] == ["--verbose", "1"]
            assert environment == {"LIBUSB_DEBUG": "4"}
            assert on_usb_ready is not None
            if mode not in {"ready", "transfer-failed"} and (
                self.attempts == 1 or mode == "still-unsupported"
            ):
                raise windows_host.BootromDriverRequiredError
            on_usb_ready()
            assert events[-1] == "ram-loader-start"
            if mode == "transfer-failed":
                windows_host.fail("native transfer failed")
            raise LoaderReachedError

    host = BootHost(consent=mode != "declined")
    monkeypatch.setattr(windows_host, "WindowsHost", lambda: host)

    # Personalization is replaced to isolate boot orchestration from key generation.
    def prepared_session(
        _host: DriverHost,
        _bundle: windows_host.WindowsBundle,
        *,
        events: object = None,
        session_root: Path,
        prepared_data_sha256: str | None = None,
    ) -> dict[str, str]:
        del events, session_root, prepared_data_sha256
        return {"directory": str(directory), "image": str(image)}

    monkeypatch.setattr(windows_host, "_prepare_session", prepared_session)
    if mode in {"ready", "setup"}:
        with pytest.raises(LoaderReachedError):
            windows_host.boot_archive(
                admitted_windows(root),
                capsule,
                session_root=root / "data/session",
                events=events.append,
            )
        prefix = ["host-preparation", "waiting-for-device"]
        if mode == "setup":
            prefix.extend(["driver-setup", "driver-reconnect", "waiting-for-device"])
        prefix.append("device-found")
        if mode == "setup":
            prefix.append("driver-ready")
        assert events == [*prefix, "ram-loader-start"]
        assert host.attempts == (2 if mode == "setup" else 1)
        assert len([effect for effect in host.effects if effect[0] == "installer"]) == (
            1 if mode == "setup" else 0
        )
    else:
        error = {
            "declined": "setup was declined",
            "still-unsupported": "WinUSB is still unavailable",
            "transfer-failed": "native transfer failed",
        }[mode]
        with pytest.raises(SystemExit, match=error):
            windows_host.boot_archive(
                admitted_windows(root),
                capsule,
                session_root=root / "data/session",
                events=events.append,
            )
        assert host.attempts == (2 if mode == "still-unsupported" else 1)
        assert len([effect for effect in host.effects if effect[0] == "installer"]) == (
            1 if mode == "still-unsupported" else 0
        )
        if mode == "declined":
            assert events == ["host-preparation", "waiting-for-device"]
    assert not image.exists()

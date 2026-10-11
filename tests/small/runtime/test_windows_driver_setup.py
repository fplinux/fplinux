# SPDX-License-Identifier: GPL-2.0-only
"""Test driver decisions with controlled Windows consent, UAC and device boundaries."""

from __future__ import annotations

import json
import subprocess
from typing import TYPE_CHECKING, Any

import pytest

from common import windows_host
from common.windows_host import validate_archive
from tests.windows_host_support import StubWindowsHost, archive_files

if TYPE_CHECKING:
    from pathlib import Path


class DriverHost(StubWindowsHost):
    """Replace native driver effects without executing an installer or accessing USB."""

    def __init__(
        self,
        *,
        service: str = "",
        consent: bool = True,
        setup_error: str | None = None,
        binds: bool = True,
    ) -> None:
        """Keep one selected synthetic BootROM and explicit user/setup outcomes."""
        super().__init__(
            {"devices": [{"id": "USB\\VID_1782&PID_4D00\\selected", "service": service}]}
        )
        self.consent = consent
        self.setup_error = setup_error
        self.binds = binds
        self.effects: list[tuple[str, Any]] = []

    def confirm_driver_install(self, license_path: Path) -> bool:
        """Model the native dialog result; this does not establish real GUI behavior."""
        self.effects.append(("consent", license_path.read_bytes()))
        return self.consent

    def install_bootrom_driver(
        self, helper: Path, directory: Path, *, log: Path | None = None
    ) -> None:
        """Model a completed elevated setup or a cancelled/failed native boundary."""
        del log
        self.effects.append(("installer", (helper, directory)))
        if self.setup_error is not None:
            windows_host.fail(self.setup_error)
        if self.binds:
            self.snapshot["devices"][0]["service"] = "WinUSB"


def ensure_driver(host: DriverHost, root: Path, events: list[str]) -> None:
    """Exercise the production decision with a real validated temporary archive."""
    bundle = validate_archive(archive_files(root))
    directory = root / "private-session"
    directory.mkdir()
    windows_host._ensure_bootrom_driver(  # noqa: SLF001 -- selected host component boundary.
        host,
        bundle,
        "USB\\VID_1782&PID_4D00\\selected",
        directory,
        snapshot=host.snapshot,
        events=events.append,
    )


def test_bound_winusb_needs_no_consent_or_installation(tmp_path: Path) -> None:
    host = DriverHost(service="wInUsB")
    events: list[str] = []
    ensure_driver(host, tmp_path, events)
    assert host.effects == []
    assert events == []


def test_declined_terms_stop_before_any_installer(tmp_path: Path) -> None:
    host = DriverHost(consent=False)
    events: list[str] = []
    with pytest.raises(SystemExit, match=r"setup was declined.*restart Run[.]cmd"):
        ensure_driver(host, tmp_path, events)
    assert host.effects == [("consent", b"{\\rtf1 Synthetic Microsoft license terms.}")]
    assert events == []


@pytest.mark.parametrize("service", ["", "OtherUsbDriver"])
def test_accepted_setup_checks_live_binding_before_ready(tmp_path: Path, service: str) -> None:
    host = DriverHost(service=service)
    events: list[str] = []
    ensure_driver(host, tmp_path, events)
    assert host.effects == [
        ("consent", b"{\\rtf1 Synthetic Microsoft license terms.}"),
        ("installer", (tmp_path / "tools/driver/wdi-simple.exe", tmp_path / "private-session")),
    ]
    assert host.snapshot["devices"][0]["service"] == "WinUSB"
    assert events == ["driver-setup", "driver-ready"]


@pytest.mark.parametrize("error", ["setup was not authorized", "setup failed with status 7"])
def test_cancelled_or_failed_setup_never_reports_ready(tmp_path: Path, error: str) -> None:
    host = DriverHost(setup_error=error)
    events: list[str] = []
    with pytest.raises(SystemExit, match=error):
        ensure_driver(host, tmp_path, events)
    assert events == ["driver-setup"]
    assert len([effect for effect in host.effects if effect[0] == "installer"]) == 1


def test_success_status_without_winusb_binding_is_not_ready(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    host = DriverHost(binds=False)
    now = 0.0

    def advance_clock(delay: float) -> None:
        nonlocal now
        now += delay

    monkeypatch.setattr("common.windows_host.time.monotonic", lambda: now)
    monkeypatch.setattr("common.windows_host.time.sleep", advance_clock)
    events: list[str] = []
    with pytest.raises(SystemExit, match=r"WinUSB did not become ready.*restart Run[.]cmd"):
        ensure_driver(host, tmp_path, events)
    assert events == ["driver-setup"]
    assert now >= 20


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


def test_driver_license_checksum_is_required_before_windows_effects(tmp_path: Path) -> None:
    root = archive_files(tmp_path)
    (root / "licenses/windows-driver/License.rtf").write_bytes(b"replaced terms")
    with pytest.raises(SystemExit, match="Windows file checksum differs: licenses/windows-driver"):
        validate_archive(root)


@pytest.mark.parametrize("consent", [True, False])
def test_boot_invites_connection_then_requires_driver_before_loader(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, *, consent: bool
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
        """Connect the synthetic phone only after the observable invitation."""

        def ps(self, script: str, *arguments: str, timeout: float = 20) -> dict[str, Any]:
            del script, arguments, timeout
            return self.snapshot if "waiting-for-device" in events else {"devices": []}

        def run_tool(
            self,
            argv: list[str],
            *,
            timeout: int,
            cwd: Path,
            log: Path,
            environment: dict[str, str] | None = None,
        ) -> None:
            del timeout, cwd, log
            self.effects.append(("loader", argv[0]))
            assert self.snapshot["devices"][0]["service"] == "WinUSB"
            assert events[-1] == "ram-loader-start"
            assert argv[1:3] == ["--verbose", "1"]
            assert environment == {"LIBUSB_DEBUG": "4"}
            raise LoaderReachedError

    host = BootHost(consent=consent)
    monkeypatch.setattr(windows_host, "WindowsHost", lambda: host)

    # Personalization is replaced to isolate boot orchestration from key generation.
    def prepared_session(
        _host: DriverHost,
        _bundle: windows_host.WindowsBundle,
        *,
        events: object = None,
        session_root: Path | None = None,
    ) -> dict[str, str]:
        del events, session_root
        return {"directory": str(directory), "image": str(image)}

    monkeypatch.setattr(windows_host, "_prepare_session", prepared_session)
    if consent:
        with pytest.raises(LoaderReachedError):
            windows_host.boot_archive(root, capsule, events=events.append)
        assert host.effects[-1] == ("loader", str(root / "tools/spd_dump.exe"))
        assert events == [
            "host-preparation",
            "waiting-for-device",
            "device-found",
            "driver-setup",
            "driver-ready",
            "ram-loader-start",
        ]
    else:
        with pytest.raises(SystemExit, match="setup was declined"):
            windows_host.boot_archive(root, capsule, events=events.append)
        assert all(effect[0] != "loader" for effect in host.effects)
        assert events == ["host-preparation", "waiting-for-device", "device-found"]
    assert not image.exists()

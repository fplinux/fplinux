# SPDX-License-Identifier: GPL-2.0-only
"""Shared reconnect coordination with private state and controlled USB/SSH endpoints."""

from __future__ import annotations

import io
import json
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.runtime import boot, ssh_transport, workflow
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.connection import ConnectionLostError, SessionResumedError
from fplinux_cli.runtime.events import RunLog

from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
    from concurrent.futures import Future


@pytest.fixture
def admitted_bundle(tmp_path: Path) -> ValidatedBundle:
    """Supply the admitted selection without reading a hardware-specific image."""
    return ValidatedBundle(
        tmp_path,
        {
            "target": "phone",
            "profile": None,
            "build_type": "release",
            "device_data": None,
            "identity": {
                "target": {"display_name": "Test phone"},
                "platform": {"name": "sc9820e"},
            },
        },
        "a" * 64,
        "12" * 32,
        None,
    )


@pytest.fixture
def saved_session(tmp_path: Path) -> dict[str, Any]:
    """Own valid saved credentials consumed by the real retained-session reader."""
    session = create_ready_session(tmp_path / "data/session", session_id=bytes.fromhex("ab" * 32))
    session.update(bundle_generation="a" * 64, prepared_data_sha256=None)
    directory = Path(session["private_key"]).parent
    (directory / "session.json").write_text(json.dumps(session), encoding="utf-8")
    current = tmp_path / "data/session/current"
    current.mkdir(mode=0o700)
    record = current / "phone.json"
    record.write_text(json.dumps(session), encoding="utf-8")
    record.chmod(0o600)
    return session


@pytest.fixture
def runtime_log(tmp_path: Path) -> Iterator[RunLog]:
    """Retain real user-facing reconnect messages without borrowing terminal streams."""
    root = tmp_path / "logs"
    root.mkdir()
    log = RunLog(root, protect=lambda _path: None, visible=io.StringIO())
    try:
        yield log
    finally:
        log.close("success")


def test_lost_shell_reauthenticates_original_session_and_reopens_without_rebooting(
    tmp_path: Path,
    admitted_bundle: ValidatedBundle,
    saved_session: dict[str, Any],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The real launcher resumes saved keys with controlled bundle and USB/SSH boundaries."""
    data = tmp_path / "data"
    before = {
        path.relative_to(data): path.read_bytes() for path in data.rglob("*") if path.is_file()
    }
    shells: list[tuple[str, str, str]] = []
    authenticated: list[tuple[str, str]] = []
    key = Path(saved_session["private_key"])

    def shell(session: dict[str, Any]) -> int:
        shells.append((session["session_id"], session["private_key"], session["interface"]))
        if len(shells) == 1:
            raise ConnectionLostError
        return 0

    def reacquire(session: dict[str, Any], *, expected_device_identity: str) -> dict[str, Any]:
        authenticated.append((session["session_id"], expected_device_identity))
        interface = "usb0" if len(authenticated) == 1 else "usb7"
        return {**session, "interface": interface}

    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted_bundle)
    monkeypatch.setattr(workflow, "host_preflight", lambda: None)
    monkeypatch.setattr(boot, "host_preflight", lambda: None)
    monkeypatch.setattr(ssh_transport, "run_shell", shell)
    monkeypatch.setattr(ssh_transport, "reacquire_bound_session", reacquire)

    result = workflow.run_phone(tmp_path, data, tmp_path / "logs", reconnect=True)

    assert result == 0
    assert shells == [("ab" * 32, str(key), "usb0"), ("ab" * 32, str(key), "usb7")]
    assert authenticated == [("ab" * 32, "12" * 32), ("ab" * 32, "12" * 32)]
    after = {
        path.relative_to(data): path.read_bytes() for path in data.rglob("*") if path.is_file()
    }
    assert {
        path: contents for path, contents in after.items() if path.name != ".launcher.lock"
    } == before
    directory = next((tmp_path / "logs").iterdir())
    assert json.loads((directory / "run.json").read_text())["status"] == "success"
    messages = (directory / "runtime.log").read_text(encoding="utf-8")
    assert (
        "SSH connection lost. Waiting for the running phone; reconnect USB if needed. "
        "Press Ctrl+C to stop."
    ) in messages
    assert "The running phone is connected again." in messages
    assert messages.count("Opening SSH.") == 2
    assert "Starting FPLinux..." not in messages


@pytest.mark.parametrize("shell_status", [0, 17], ids=["exit", "remote-failure"])
def test_normal_shell_exit_ends_terminal_without_reconnecting(
    admitted_bundle: ValidatedBundle,
    saved_session: dict[str, Any],
    runtime_log: RunLog,
    monkeypatch: pytest.MonkeyPatch,
    shell_status: int,
) -> None:
    """A normal SSH status, including failure, is never interpreted as connection loss."""
    opened: list[str] = []

    def shell(session: dict[str, Any]) -> int:
        opened.append(session["session_id"])
        return shell_status

    def unexpected_reconnect(*_args: object, **_kwargs: object) -> dict[str, Any]:
        pytest.fail("a normal SSH exit must not reconnect")

    monkeypatch.setattr(ssh_transport, "run_shell", shell)
    monkeypatch.setattr(ssh_transport, "reacquire_bound_session", unexpected_reconnect)
    if shell_status:
        with pytest.raises(SystemExit) as result:
            workflow._interactive_shell(  # noqa: SLF001 -- shared terminal lifetime boundary.
                admitted_bundle,
                (ssh_transport, saved_session),
                runtime_log,
                prepared_data_sha256=None,
            )
        assert result.value.code == 17
    else:
        workflow._interactive_shell(  # noqa: SLF001 -- shared terminal lifetime boundary.
            admitted_bundle,
            (ssh_transport, saved_session),
            runtime_log,
            prepared_data_sha256=None,
        )
    assert opened == ["ab" * 32]
    assert "SSH connection lost." not in runtime_log.path.read_text(encoding="utf-8")


@pytest.mark.parametrize(
    ("reconnect_error", "expected_status", "journal_status"),
    [
        (subprocess.TimeoutExpired("reconnect", 30), 1, "failed"),
        (KeyboardInterrupt(), 130, "interrupted"),
    ],
    ids=["timeout", "cancel"],
)
def test_terminal_reconnect_failure_preserves_saved_keys_without_rebooting(  # noqa: PLR0913 -- fixtures and failure policies.
    tmp_path: Path,
    admitted_bundle: ValidatedBundle,
    saved_session: dict[str, Any],
    monkeypatch: pytest.MonkeyPatch,
    *,
    reconnect_error: BaseException,
    expected_status: int,
    journal_status: str,
) -> None:
    """The public wrapper reports bounded failure and leaves the previous boot reusable."""
    data = tmp_path / "data"
    before = {
        path.relative_to(data): path.read_bytes() for path in data.rglob("*") if path.is_file()
    }
    authentications: list[str] = []
    shells: list[str] = []

    def reacquire(session: dict[str, Any], *, expected_device_identity: str) -> dict[str, Any]:
        assert expected_device_identity == "12" * 32
        authentications.append(session["session_id"])
        if len(authentications) == 2:
            raise reconnect_error
        return session

    def shell(session: dict[str, Any]) -> int:
        shells.append(session["session_id"])
        raise ConnectionLostError

    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted_bundle)
    monkeypatch.setattr(workflow, "host_preflight", lambda: None)
    monkeypatch.setattr(boot, "host_preflight", lambda: None)
    monkeypatch.setattr(ssh_transport, "reacquire_bound_session", reacquire)
    monkeypatch.setattr(ssh_transport, "run_shell", shell)

    result = workflow.run_phone(tmp_path, data, tmp_path / "logs", reconnect=True)

    assert result == expected_status
    assert authentications == [saved_session["session_id"], saved_session["session_id"]]
    assert shells == [saved_session["session_id"]]
    after = {
        path.relative_to(data): path.read_bytes() for path in data.rglob("*") if path.is_file()
    }
    assert {
        path: contents for path, contents in after.items() if path.name != ".launcher.lock"
    } == before
    directory = next((tmp_path / "logs").iterdir())
    assert json.loads((directory / "run.json").read_text())["status"] == journal_status
    messages = (directory / "runtime.log").read_text(encoding="utf-8")
    assert "Starting FPLinux..." not in messages
    assert "The running phone is connected again." not in messages


def test_remote_command_failure_is_reported_once_without_reconnecting_or_replaying(
    tmp_path: Path,
    admitted_bundle: ValidatedBundle,
    saved_session: dict[str, Any],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An explicit command remains a one-shot action even when SSH returns 255."""
    commands: list[tuple[str, str]] = []
    authentications: list[str] = []

    def reacquire(session: dict[str, Any], *, expected_device_identity: str) -> dict[str, Any]:
        assert expected_device_identity == "12" * 32
        authentications.append(session["session_id"])
        return session

    def command(session: dict[str, Any], text: str) -> subprocess.CompletedProcess[str]:
        commands.append((session["session_id"], text))
        return subprocess.CompletedProcess(text, 255)

    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted_bundle)
    monkeypatch.setattr(workflow, "host_preflight", lambda: None)
    monkeypatch.setattr(boot, "host_preflight", lambda: None)
    monkeypatch.setattr(ssh_transport, "reacquire_bound_session", reacquire)
    monkeypatch.setattr(ssh_transport, "run_remote", command)

    result = workflow.run_phone(
        tmp_path, tmp_path / "data", tmp_path / "logs", reconnect=True, exec_command="printf sent"
    )

    assert result == 255
    assert commands == [("ab" * 32, "printf sent")]
    assert authentications == ["ab" * 32]
    assert Path(saved_session["private_key"]).read_bytes() == b"private\n"


@pytest.fixture
def observer_completed(monkeypatch: pytest.MonkeyPatch) -> threading.Event:
    """Observe completion while retaining the production executor's real join behavior."""
    completed = threading.Event()

    class ObservedExecutor(ThreadPoolExecutor):
        def submit[Result, **Parameters](
            self,
            function: Callable[Parameters, Result],
            /,
            *args: Parameters.args,
            **kwargs: Parameters.kwargs,
        ) -> Future[Result]:
            future = super().submit(function, *args, **kwargs)
            future.add_done_callback(lambda _future: completed.set())
            return future

    monkeypatch.setattr(boot, "ThreadPoolExecutor", ObservedExecutor)
    return completed


def test_previous_connection_callback_does_not_wait_for_a_blocked_usb_probe(
    saved_session: dict[str, Any], observer_completed: threading.Event
) -> None:
    """A native loader callback stays responsive and context exit reaps its USB observer."""
    entered = threading.Event()
    release = threading.Event()
    observer_threads: list[threading.Thread] = []
    cancellations: list[bool] = []
    state = Path(saved_session["private_key"]).parent / "session.json"
    before = state.read_bytes()

    def usb_present(_session: dict[str, Any], *, cancelled: Callable[[], bool]) -> bool:
        observer_threads.append(threading.current_thread())
        entered.set()
        assert release.wait(5), "test did not release the USB probe"
        cancellations.append(cancelled())
        return False

    transport = SimpleNamespace(usb_present=usb_present)
    with boot._watch_previous_connection((transport, saved_session)) as (on_wait, cancel):  # noqa: SLF001
        try:
            assert entered.wait(5), "USB observer did not start"
            assert on_wait is not None
            with ThreadPoolExecutor(max_workers=1) as caller:
                caller.submit(on_wait).result(timeout=1)
        finally:
            cancel()
            release.set()

    assert observer_completed.is_set()
    assert cancellations == [True]
    assert len(observer_threads) == 1
    assert not observer_threads[0].is_alive()
    assert state.read_bytes() == before


def test_previous_connection_callback_resumes_exact_saved_session_when_usb_returns(
    saved_session: dict[str, Any], observer_completed: threading.Event
) -> None:
    """The observer signals old credentials rather than creating a replacement RAM session."""
    entered = threading.Event()
    release = threading.Event()
    probed: list[str] = []
    state = Path(saved_session["private_key"]).parent / "session.json"
    before = state.read_bytes()

    def usb_present(session: dict[str, Any], *, cancelled: Callable[[], bool]) -> bool:
        probed.append(session["session_id"])
        entered.set()
        assert release.wait(5), "test did not release the returning USB device"
        return not cancelled()

    transport = SimpleNamespace(usb_present=usb_present)
    with boot._watch_previous_connection((transport, saved_session)) as (on_wait, _cancel):  # noqa: SLF001
        try:
            assert entered.wait(5), "USB observer did not start"
            release.set()
            assert observer_completed.wait(5), "USB observation did not finish"
            assert on_wait is not None
            with pytest.raises(SessionResumedError) as resumed:
                on_wait()
            assert resumed.value.session is saved_session
        finally:
            release.set()

    assert probed == ["ab" * 32]
    assert state.read_bytes() == before


def test_normal_boot_discards_unloaded_candidate_before_reauthenticating_returning_session(
    tmp_path: Path,
    admitted_bundle: ValidatedBundle,
    saved_session: dict[str, Any],
    observer_completed: threading.Event,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The real boot wrapper resumes Linux after cleaning its unused new-boot credentials."""
    session_root = tmp_path / "data/session"
    before = {path: path.read_bytes() for path in session_root.rglob("*") if path.is_file()}
    image = tmp_path / "image.bin"
    image.write_bytes(b"synthetic RAM image\n")
    admitted_bundle.runtime.update(image="image.bin", personalization={}, usb={"linux_gadget": {}})
    rules = tmp_path / "installed-usb.rules"
    rules.touch()
    probe_entered = threading.Event()
    usb_returned = threading.Event()
    candidates: list[Path] = []
    authenticated: list[str] = []
    ram_transfers: list[str] = []
    events: list[str] = []

    def prepare(
        source: Path,
        _personalization: dict[str, Any],
        target: str,
        _linux_usb: dict[str, Any],
        *,
        runtime_root: Path,
    ) -> dict[str, Any]:
        assert source == image
        assert target == "phone"
        assert runtime_root == session_root
        candidate = create_ready_session(
            runtime_root, status="prepared", session_id=bytes.fromhex("cd" * 32)
        )
        Path(candidate["image"]).write_bytes(b"personalized candidate RAM image\n")
        candidates.append(Path(candidate["private_key"]).parent)
        return candidate

    def usb_present(session: dict[str, Any], *, cancelled: Callable[[], bool]) -> bool:
        assert session["session_id"] == "ab" * 32
        probe_entered.set()
        assert usb_returned.wait(5), "test did not reconnect the old USB device"
        return not cancelled()

    class WaitingAdapter:
        def run(
            self,
            _bundle: Path,
            _runtime: dict[str, Any],
            session: dict[str, Any],
            *,
            events: Callable[[str], None],
            on_wait: Callable[[], None],
            **_options: object,
        ) -> dict[str, Any]:
            events("waiting-for-device")
            assert probe_entered.wait(5), "USB observer did not start"
            assert session["session_id"] == "cd" * 32
            assert candidates[0].is_dir()
            on_wait()
            usb_returned.set()
            assert observer_completed.wait(5), "returning USB observation did not finish"
            on_wait()
            ram_transfers.append(session["session_id"])
            pytest.fail("a returning Linux session must prevent a new RAM transfer")

    def reacquire(session: dict[str, Any], *, expected_device_identity: str) -> dict[str, Any]:
        assert expected_device_identity == "12" * 32
        assert session == saved_session
        assert len(candidates) == 1
        assert not candidates[0].exists()
        assert {
            path: path.read_bytes() for path in session_root.rglob("*") if path.is_file()
        } == before
        authenticated.append(session["session_id"])
        return session

    monkeypatch.setattr(boot, "host_preflight", lambda: None)
    monkeypatch.setattr(boot, "USB_RULES_FILE", rules)
    monkeypatch.setattr(boot, "load_adapter", lambda _path: WaitingAdapter())
    monkeypatch.setattr(ssh_transport, "prepare_session", prepare)
    monkeypatch.setattr(ssh_transport, "usb_present", usb_present)
    monkeypatch.setattr(ssh_transport, "reacquire_bound_session", reacquire)

    try:
        transport, session = boot.run_bundle(
            admitted_bundle,
            events.append,
            session_root=session_root,
            log=tmp_path / "runtime.log",
        )
    finally:
        usb_returned.set()

    assert transport is ssh_transport
    assert session == saved_session
    assert authenticated == ["ab" * 32]
    assert ram_transfers == []
    assert events == ["waiting-for-device", "session-resumed"]

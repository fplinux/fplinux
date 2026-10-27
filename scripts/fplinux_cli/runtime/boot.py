# SPDX-License-Identifier: GPL-2.0-only
"""Boot an admitted bundle or authenticate its existing USB session."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import ssh_transport
from .bundle import adapter_path, fail, host_preflight, load_adapter
from .connection import SessionResumedError

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
    from types import FrameType
    from typing import TextIO

    from .bundle import ValidatedBundle

USB_RULES_FILE = Path("/etc/udev/rules.d/60-fplinux.rules")


def prepare_usb_access(bundle: Path) -> None:
    """Install the phone access rules without replacing existing local rules."""
    if USB_RULES_FILE.exists():
        return
    rules = bundle / "60-fplinux.rules"
    if not rules.is_file():
        fail("The phone USB access rules are missing")
    sudo = shutil.which("sudo")
    udevadm = shutil.which("udevadm")
    if sudo is None or udevadm is None:
        fail("sudo and udevadm are required for the initial phone USB access setup")
    for command in (
        [sudo, "install", "-m", "0644", str(rules), str(USB_RULES_FILE)],
        [sudo, udevadm, "control", "--reload-rules"],
    ):
        result = subprocess.run(command, check=False)
        if result.returncode:
            fail("Phone USB access setup was not completed; restart the launcher to try again")


@contextmanager
def _watch_previous_connection(
    previous: tuple[Any, dict[str, Any]] | None,
) -> Iterator[tuple[Callable[[], None] | None, Callable[[], None]]]:
    """Observe the old USB device without delaying a native BootROM listener."""
    cancelled = threading.Event()
    if previous is None:
        yield None, cancelled.set
        return
    transport, session = previous

    def observe() -> bool:
        while not cancelled.is_set():
            try:
                if transport.usb_present(session, cancelled=cancelled.is_set):
                    return True
            except subprocess.TimeoutExpired:
                continue
            cancelled.wait(0.25)
        return False

    with ThreadPoolExecutor(max_workers=1, thread_name_prefix="fplinux-usb") as executor:
        future = executor.submit(observe)

        def resume_if_present() -> None:
            if future.done() and future.result():
                raise SessionResumedError(session)

        try:
            yield resume_if_present, cancelled.set
        finally:
            cancelled.set()


def run_bundle(  # noqa: PLR0913 -- boot inputs and their resource destinations are distinct.
    admitted: ValidatedBundle,
    emit: Callable[[str], None],
    *,
    device_data: Path | None = None,
    prepared_data_sha256: str | None = None,
    output: TextIO | None = None,
    session_root: Path,
    loader_trace: bool = False,
    log: Path,
) -> tuple[Any, dict[str, Any]]:
    """Select a returning Linux session or a positively detected fresh BootROM."""
    previous = saved_bundle_connection(admitted, session_root=session_root)
    resumed: tuple[Any, dict[str, Any]]
    with _watch_previous_connection(previous) as (on_wait, stop_watching):

        def progress(stage: str) -> None:
            if stage in {"device-found", "ram-loader-start"}:
                stop_watching()
            emit(stage)

        try:
            connection = _run_new_bundle(
                admitted,
                progress,
                device_data=device_data,
                prepared_data_sha256=prepared_data_sha256,
                output=output,
                session_root=session_root,
                loader_trace=loader_trace,
                log=log,
                on_wait=on_wait,
            )
        except SessionResumedError as found:
            if previous is None:
                raise
            resumed = previous[0], found.session
        else:
            return connection
    connection = reconnect_saved_connection(
        admitted, resumed, prepared_data_sha256=prepared_data_sha256
    )
    emit("session-resumed")
    return connection


def _run_new_bundle(  # noqa: PLR0913 -- the waiting observer preserves the admitted boot inputs.
    admitted: ValidatedBundle,
    emit: Callable[[str], None],
    *,
    device_data: Path | None = None,
    prepared_data_sha256: str | None = None,
    output: TextIO | None = None,
    session_root: Path,
    loader_trace: bool = False,
    log: Path,
    on_wait: Callable[[], None] | None = None,
) -> tuple[Any, dict[str, Any]]:
    """Load RAM and keep keys only when the authenticated handoff succeeds."""
    host_preflight()
    bundle, runtime = admitted.root, admitted.runtime
    if os.name == "nt":
        from .windows_host import boot_archive  # noqa: PLC0415

        if device_data is None:
            fail("This Windows phone requires its RAM data capsule")
        if admitted.windows is None:
            fail("This bundle has no admitted Windows host tools")
        return boot_archive(
            admitted.windows,
            device_data,
            prepared_data_sha256=prepared_data_sha256,
            events=emit,
            log=log,
            session_root=session_root,
            on_wait=on_wait,
        )
    image = bundle / runtime["image"]
    with image.open("rb") as stream:
        header = stream.read(4)
    if runtime["identity"]["platform"]["name"] == "ums9117" and header != b"DHTB":
        fail("RAM payload does not have a DHTB header")
    adapter = load_adapter(bundle / adapter_path(runtime))
    session = None
    previous_handlers: dict[signal.Signals, Any] = {}

    def stop_session(signum: int, _frame: FrameType | None) -> None:
        raise SystemExit(128 + signum)

    for signum in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, stop_session)
    try:
        session = ssh_transport.prepare_session(
            image,
            runtime["personalization"],
            runtime["target"],
            runtime["usb"]["linux_gadget"],
            runtime_root=session_root,
        )
        ssh_transport.bind_prepared_content(
            session,
            bundle_generation=admitted.generation,
            prepared_data_sha256=prepared_data_sha256,
        )
        if not USB_RULES_FILE.exists():
            emit("usb-access-setup")
            prepare_usb_access(bundle)
        platform_options: dict[str, Any] = {}
        if loader_trace:
            platform_options["loader_trace"] = True
        if on_wait is not None:
            platform_options["on_wait"] = on_wait
        ready: dict[str, Any] = adapter.run(
            bundle,
            runtime,
            session,
            expected_device_identity=admitted.device_identity,
            events=emit,
            device_data=device_data,
            output=output,
            **platform_options,
        )
    finally:
        original_error = sys.exception()
        try:
            if session is not None:
                ssh_transport.finish_session(session)
        except OSError, SystemExit:
            if original_error is None:
                raise
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
    emit("complete")
    return ssh_transport, ready


def saved_bundle_connection(
    admitted: ValidatedBundle,
    *,
    session_root: Path,
) -> tuple[Any, dict[str, Any]] | None:
    """Read retained credentials without mistaking temporary USB absence for power loss."""
    host_preflight()
    runtime = admitted.runtime
    if os.name == "nt":
        from .windows_host import WindowsHost, WindowsTransport  # noqa: PLC0415

        if admitted.windows is None:
            fail("This bundle has no admitted Windows host tools")
        transport: Any = WindowsTransport(WindowsHost(), admitted.windows)
        session = transport.load_saved_session(runtime["target"], session_root=session_root)
    else:
        transport = ssh_transport
        session = ssh_transport.load_saved_session(runtime["target"], runtime_root=session_root)
    return (transport, session) if session is not None else None


def reconnect_saved_connection(
    admitted: ValidatedBundle,
    connection: tuple[Any, dict[str, Any]],
    *,
    prepared_data_sha256: str | None = None,
) -> tuple[Any, dict[str, Any]]:
    """Authenticate the original keys, RAM identity and selected build after a replug."""
    transport, session = connection
    if transport is ssh_transport:
        ssh_transport.require_prepared_content(
            session,
            bundle_generation=admitted.generation,
            prepared_data_sha256=prepared_data_sha256,
        )
    elif session["prepared_data_sha256"] != prepared_data_sha256:
        fail("The running phone uses different prepared phone data; shut it down first")
    session = transport.reacquire_bound_session(
        session, expected_device_identity=admitted.device_identity
    )
    return transport, session


def reconnect_live_bundle(
    admitted: ValidatedBundle,
    *,
    session_root: Path,
    prepared_data_sha256: str | None = None,
    required: bool = False,
) -> tuple[Any, dict[str, Any]] | None:
    """Wait for a requested saved session; otherwise probe its current USB presence."""
    connection = saved_bundle_connection(admitted, session_root=session_root)
    if connection is None:
        if required:
            fail("No saved SSH session is available; start the powered-off phone first")
        return None
    transport, session = connection
    if not required and not transport.usb_present(session):
        return None
    return reconnect_saved_connection(
        admitted, connection, prepared_data_sha256=prepared_data_sha256
    )

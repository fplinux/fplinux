# SPDX-License-Identifier: GPL-2.0-only
"""Select and authenticate an existing phone without starting a new boot."""

from __future__ import annotations

import os
import subprocess
import sys
from contextlib import nullcontext
from typing import TYPE_CHECKING, Any

from . import boot
from . import phone_data as phone_data_runtime
from .bundle import admit_bundle, fail, host_preflight, load_runtime_manifest
from .connection import read_running_build
from .events import RunLog
from .ssh import session as ssh_session
from .ssh import transport as ssh_transport
from .state import exclusive_lock, private_directory
from .workflow import (
    cancellation_signals,
    interactive_shell,
    report_runtime_error,
)

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable
    from pathlib import Path

    from .bundle import ValidatedBundle


def load_connections(
    target: str,
    session_root: Path,
    *,
    bundle_root: Path | None = None,
    runtime: dict[str, Any] | None = None,
) -> list[tuple[Any, dict[str, Any]]]:
    """Read saved credentials from this installation without creating a session."""
    if not session_root.exists():
        return []
    if os.name == "nt":
        from .windows.bundle import validate_connection_tools  # noqa: PLC0415
        from .windows.host import WindowsHost  # noqa: PLC0415
        from .windows.transport import WindowsTransport  # noqa: PLC0415

        if bundle_root is None or runtime is None:
            fail("Windows connection requires its archive and runtime metadata")
        tools = validate_connection_tools(bundle_root)
        transport = WindowsTransport(
            WindowsHost(), tools, wait_seconds=runtime["usb"]["linux_gadget"]["wait_seconds"]
        )
        return [
            (transport, session)
            for session in transport.load_saved_sessions(target, session_root=session_root)
        ]
    session = ssh_session.load_saved_session(target, runtime_root=session_root)
    return [(ssh_transport, session)] if session is not None else []


def select_connection(
    candidates: Iterable[tuple[Any, dict[str, Any]]],
) -> tuple[Any, dict[str, Any]]:
    """Select only an exact connected USB binding, never the most recent record."""
    live = [
        (transport, session) for transport, session in candidates if transport.usb_present(session)
    ]
    if not live:
        fail("No known live phone connection is available; start the powered-off phone first")
    if len(live) > 1:
        targets = ", ".join(sorted({session["target"] for _, session in live}))
        fail(f"More than one live phone connection is available ({targets}); select a target")
    return live[0]


def _prepared_identity(admitted: ValidatedBundle, phone_data: Path | None) -> str | None:
    spec = phone_data_runtime.device_data_spec(admitted.root, admitted.runtime)
    selected = None
    if spec is not None:
        from fplinux_cli.device_data.store import locked_phone_store  # noqa: PLC0415

        with locked_phone_store(admitted.runtime["target"], root=phone_data) as store:
            if spec.required_declarations and not phone_data_runtime.phone_preparation_current(
                admitted, spec, store
            ):
                fail("Prepare the phone data before reconnecting")
            selected = phone_data_runtime.selected_phone_data(spec, store / "phone")
    return phone_data_runtime.content_identity(phone_data_runtime.selected_capsule(selected))


def _show_running_build(connection: tuple[Any, dict[str, Any]], log: RunLog) -> None:
    observed = read_running_build(connection)
    log.message(
        f"Phone: {connection[1]['target']}; kernel: {observed.kernel_release}; "
        f"BUILD_ID: {observed.build_id}"
    )


def console_connection(  # noqa: PLR0913 -- selected session and action policies stay explicit.
    connection: tuple[Any, dict[str, Any]],
    data_root: Path,
    log_root: Path,
    *,
    admitted: ValidatedBundle | None,
    phone_data: Path | None = None,
    exec_command: str | None = None,
    upload: list[str] | None = None,
    pull: list[str] | None = None,
    verify_only: bool = False,
    revalidate: Callable[[], None] | None = None,
    log: RunLog | None = None,
) -> int:
    """Authenticate one selected session and perform at most one remote action."""
    status = "failed"
    try:
        private_directory(data_root)
        remote_action = verify_only or any(
            value is not None for value in (exec_command, upload, pull)
        )
        lock = (
            nullcontext()
            if remote_action
            else exclusive_lock(
                data_root / ".launcher.lock",
                message="Another launcher is already open. Close it before starting this one.",
            )
        )
        with cancellation_signals(), lock:
            if log is None:
                private_directory(log_root)
                log = RunLog(log_root, protect=private_directory, visible=sys.stderr)
                log.message(f"Log: {log.path}")
            if verify_only and (
                admitted is None or any(x is not None for x in (exec_command, upload, pull))
            ):
                fail("Verification requires a selected build and no remote action")
            if os.name == "nt" and (upload is not None or pull is not None):
                fail("File transfer is available from a Linux host")
            if admitted is None:
                log.message("Diagnostic connection: the current build is not being verified.")
            identity: str | None = None
            identity_captured = False

            def reconnect(current: tuple[Any, dict[str, Any]]) -> tuple[Any, dict[str, Any]]:
                nonlocal identity, identity_captured
                if revalidate is not None:
                    revalidate()
                if admitted is not None:
                    if current[1]["target"] != admitted.runtime["target"]:
                        fail("The selected build belongs to another phone target")
                    current_identity = _prepared_identity(admitted, phone_data)
                    if identity_captured and current_identity != identity:
                        fail("The selected phone data changed while the console was open")
                    identity, identity_captured = current_identity, True
                    return boot.reconnect_saved_connection(
                        admitted, current, prepared_data_sha256=identity
                    )
                transport, session = current
                return transport, transport.reacquire_bound_session(
                    session, expected_device_identity=None
                )

            transport, session = connection
            with log.capture():
                session = transport.reacquire_bound_session(session, expected_device_identity=None)
            connection = transport, session
            _show_running_build(connection, log)
            if admitted is not None:
                with log.capture():
                    connection = reconnect(connection)
            transport, session = connection
            if verify_only:
                log.message("The running phone matches the selected build.")
            elif exec_command is not None:
                result = transport.run_remote(session, exec_command)
                if result.returncode:
                    raise SystemExit(result.returncode)  # noqa: TRY301 -- preserve remote status.
            elif upload is not None:
                transport.upload(session, *upload)
            elif pull is not None:
                transport.pull(session, *pull)
            else:
                interactive_shell(connection, log, reconnect=reconnect)
            status = "success"
            return 0
    except (
        KeyboardInterrupt,
        OSError,
        ValueError,
        SystemExit,
        subprocess.TimeoutExpired,
    ) as error:
        status, code = report_runtime_error(error, log)
        return code
    finally:
        if log is not None:
            log.close(status)


def console_bundle(  # noqa: PLR0913 -- archive selectors and remote actions are independent.
    bundle: Path,
    data_root: Path,
    log_root: Path,
    *,
    allow_other_build: bool = False,
    exec_command: str | None = None,
    upload: list[str] | None = None,
    pull: list[str] | None = None,
    phone_data: Path | None = None,
    verify_only: bool = False,
) -> int:
    """Select this archive's saved phone and apply its chosen build policy."""
    log = None
    try:
        with cancellation_signals():
            private_directory(data_root)
            private_directory(log_root)
            log = RunLog(log_root, protect=private_directory, visible=sys.stderr)
            log.message(f"Log: {log.path}")
            host_preflight()
            runtime = load_runtime_manifest(bundle / "runtime-manifest.json")
            connection = select_connection(
                load_connections(
                    runtime["target"], data_root / "session", bundle_root=bundle, runtime=runtime
                )
            )
            admitted = None if allow_other_build else admit_bundle(bundle)
    except (
        KeyboardInterrupt,
        OSError,
        ValueError,
        SystemExit,
        subprocess.TimeoutExpired,
    ) as error:
        status, code = report_runtime_error(error, log)
        if log is not None:
            log.close(status)
        return code
    return console_connection(
        connection,
        data_root,
        log_root,
        admitted=admitted,
        phone_data=phone_data,
        exec_command=exec_command,
        upload=upload,
        pull=pull,
        verify_only=verify_only,
        log=log,
    )

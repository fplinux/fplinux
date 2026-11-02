# SPDX-License-Identifier: GPL-2.0-only
"""Own launch diagnostics and the interactive SSH lifetime."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import traceback
from contextlib import contextmanager, nullcontext
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import replace_file_atomically

from . import boot
from . import phone_data as phone_data_runtime
from .bundle import admit_bundle, fail, host_preflight
from .connection import ConnectionLostError
from .events import RunLog, record_events
from .state import exclusive_lock, private_directory

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
    from types import FrameType

    from .bundle import ValidatedBundle


@contextmanager
def cancellation_signals() -> Iterator[None]:
    """Let preparation and journal cleanup run before an external cancellation exits."""

    def cancel(signum: int, _frame: FrameType | None) -> None:
        if signum == signal.SIGINT:
            raise KeyboardInterrupt
        raise SystemExit(128 + signum)

    signals = [signal.SIGINT, signal.SIGTERM]
    if hasattr(signal, "SIGHUP"):
        signals.append(signal.SIGHUP)
    previous = {signum: signal.signal(signum, cancel) for signum in signals}
    try:
        yield
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def _interactive_shell(
    admitted: ValidatedBundle,
    connection: tuple[Any, dict[str, Any]],
    log: RunLog,
    *,
    prepared_data_sha256: str | None,
) -> None:
    """Reauthenticate interrupted interactive SSH without replaying remote commands."""
    while True:
        transport, session = connection
        log.message("Opening SSH. Type exit or press Ctrl+D to close the terminal.")
        try:
            status = transport.run_shell(session)
        except ConnectionLostError:
            log.event("connection-lost", invitation="")
            with log.capture():
                connection = boot.reconnect_saved_connection(
                    admitted, connection, prepared_data_sha256=prepared_data_sha256
                )
            log.event("session-resumed", invitation="")
            continue
        if status:
            raise SystemExit(status)
        log.message("SSH terminal closed. Open the same launcher to connect again.")
        return


def _run_phone(  # noqa: PLR0913 -- runtime actions and explicit paths preserve CLI contracts.
    admitted: ValidatedBundle,
    data: Path,
    log: RunLog,
    emit: Callable[[str], None],
    *,
    phone_data: Path | None,
    from_dump: Path | None,
    prepare_only: bool,
    replace: bool,
    reconnect: bool,
    exec_command: str | None,
    upload: list[str] | None,
    pull: list[str] | None,
    loader_trace: bool,
) -> None:
    bundle, runtime = admitted.root, admitted.runtime
    log.message(f"FPLinux — {runtime['identity']['target']['display_name']}")
    if os.name == "nt" and (upload is not None or pull is not None):
        fail("File transfer is available from a Linux host")
    spec = phone_data_runtime.device_data_spec(bundle, runtime)
    selected = None
    if spec is not None:
        from fplinux_cli.device_data.store import locked_phone_store  # noqa: PLC0415

        with locked_phone_store(runtime["target"], root=phone_data) as store:
            if reconnect:
                if not phone_data_runtime.phone_preparation_current(admitted, spec, store):
                    fail("Prepare the phone data before reconnecting")
            else:
                reader_used = phone_data_runtime.prepare_phone_data(
                    admitted,
                    data,
                    log,
                    emit,
                    store=store,
                    from_dump=from_dump,
                    replace=replace,
                    loader_trace=loader_trace,
                )
                if prepare_only or reader_used:
                    return
            selected = phone_data_runtime.selected_phone_data(spec, store / "phone")
    elif prepare_only or from_dump is not None:
        fail("This bundle boots directly and does not require phone-data preparation")
    if not reconnect and (data / "reader").exists():
        with log.capture():
            reader_connection = boot.reconnect_live_bundle(
                admitted,
                session_root=data / "reader/session",
                prepared_data_sha256=phone_data_runtime.content_identity(
                    phone_data_runtime.capsule_bytes({}, preparation=True)
                ),
            )
        if reader_connection is not None:
            with phone_data_runtime.preparation_progress(reader_connection, log) as progress:
                phone_data_runtime.finish_reader(reader_connection, data, log, progress=progress)
            return
        shutil.rmtree(data / "reader")
    contents = phone_data_runtime.selected_capsule(selected)
    identity = phone_data_runtime.content_identity(contents)
    log.message("Checking the saved phone connection...")
    with log.capture():
        connection = boot.reconnect_live_bundle(
            admitted,
            session_root=data / "session",
            prepared_data_sha256=identity,
            required=reconnect,
        )
    if connection is None:
        log.message("Starting FPLinux...")
        with tempfile.TemporaryDirectory(prefix=".boot-", dir=data) as temporary:
            staging = private_directory(Path(temporary))
            capsule = None
            if contents is not None:
                capsule = staging / "device-data.cpio"
                replace_file_atomically(capsule, contents, 0o600)
            connection = boot.boot_connection(
                admitted,
                data / "session",
                log,
                emit,
                capsule=capsule,
                prepared_data_sha256=identity,
                loader_trace=loader_trace,
            )
    else:
        log.message("Connecting to the running phone...")
    transport, session = connection
    if exec_command is not None:
        result = transport.run_remote(session, exec_command)
        if result.returncode:
            raise SystemExit(result.returncode)
    elif upload is not None:
        transport.upload(session, *upload)
    elif pull is not None:
        transport.pull(session, *pull)
    elif not reconnect and not os.isatty(0):
        log.message("No interactive terminal is attached; the loader is complete.")
    else:
        _interactive_shell(admitted, connection, log, prepared_data_sha256=identity)


def run_phone(  # noqa: PLR0913 -- explicit CLI policies keep preparation and remote actions distinct.
    bundle: Path,
    data_root: Path,
    log_root: Path,
    *,
    phone_data: Path | None = None,
    from_dump: Path | None = None,
    prepare_only: bool = False,
    replace: bool = False,
    reconnect: bool = False,
    exec_command: str | None = None,
    upload: list[str] | None = None,
    pull: list[str] | None = None,
    events: Path | None = None,
    loader_trace: bool = False,
    nand_action: str | None = None,
    nand_output: Path | None = None,
) -> int:
    """Run the shared workflow with diagnostics and one state lock through SSH exit."""
    log = None
    status = "failed"
    try:
        data = private_directory(data_root)
        remote_action = nand_action is not None or any(
            value is not None for value in (exec_command, upload, pull)
        )
        lock = (
            nullcontext()
            if remote_action
            else exclusive_lock(
                data / ".launcher.lock",
                message="Another launcher is already open. Close it before starting this one.",
            )
        )
        with lock, cancellation_signals():
            private_directory(log_root)
            log = RunLog(
                log_root,
                protect=private_directory,
                visible=sys.stderr if remote_action else sys.stdout,
            )
            log.message(f"Log: {log.path}")
            host_preflight()
            admitted = admit_bundle(bundle)
            runtime = admitted.runtime
            current_log = log
            with record_events(
                events,
                target=runtime["target"],
                profile=runtime["profile"],
                build_type=runtime["build_type"],
            ) as loader_events:

                def emit(stage: str) -> None:
                    current_log.event(stage, invitation=runtime["adapter"]["boot_instructions"])
                    loader_events.emit(stage)

                if nand_action is not None:
                    phone_data_runtime.run_nand(
                        admitted,
                        data,
                        log,
                        action=nand_action,
                        output=nand_output,
                        phone_data=phone_data,
                    )
                else:
                    _run_phone(
                        admitted,
                        data,
                        log,
                        emit,
                        phone_data=phone_data,
                        from_dump=from_dump,
                        prepare_only=prepare_only,
                        replace=replace,
                        reconnect=reconnect,
                        exec_command=exec_command,
                        upload=upload,
                        pull=pull,
                        loader_trace=loader_trace,
                    )
            status = "success"
            return 0
    except KeyboardInterrupt:
        status = "interrupted"
        if log is not None:
            log.message("Cancelled. Cancelling does not power the phone off.")
        return 130
    except (OSError, ValueError, SystemExit, subprocess.TimeoutExpired) as error:
        if isinstance(error, SystemExit) and error.code in (129, 130, 143):
            status = "interrupted"
            if log is not None:
                log.message("Cancelled. Cancelling does not power the phone off.")
            return int(error.code)
        if log is not None:
            traceback.print_exception(error, file=log.stream)
        detail = (
            f"Computer connection setup timed out after {error.timeout:g} seconds."
            if isinstance(error, subprocess.TimeoutExpired)
            else str(error)
        )
        for prefix in ("FPLinux: ", "fplinux: ", "fplinux run: ", "fplinux ssh: "):
            detail = detail.removeprefix(prefix)
        print(f"FPLinux: {detail}", file=sys.stderr, flush=True)
        if log is not None:
            print(f"See {log.path} for details.", file=sys.stderr, flush=True)
        return error.code if isinstance(error, SystemExit) and isinstance(error.code, int) else 1
    finally:
        if log is not None:
            log.close(status)

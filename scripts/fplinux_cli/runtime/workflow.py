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
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import replace_file_atomically

from . import boot
from . import phone_data as phone_data_runtime
from .bundle import admit_bundle, boots_system_card, fail, host_preflight
from .connection import ConnectionLostError
from .events import RunLog, record_events
from .state import exclusive_lock, private_directory

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
    from types import FrameType

    from fplinux_cli.device_data.inputs import FirmwareInput

    from .bundle import ValidatedBundle

    CardInspector = Callable[[tuple[Any, dict[str, Any]], RunLog], None]
    CardInstaller = Callable[
        [tuple[Any, dict[str, Any]], dict[str, tuple[FirmwareInput, ...]], RunLog], None
    ]


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


def report_runtime_error(error: BaseException, log: RunLog | None) -> tuple[str, int]:
    """Report one runtime failure and return its journal status and exit code."""
    if isinstance(error, KeyboardInterrupt) or (
        isinstance(error, SystemExit) and error.code in (129, 130, 143)
    ):
        if log is not None:
            log.message("Cancelled. Cancelling does not power the phone off.")
        code = error.code if isinstance(error, SystemExit) and isinstance(error.code, int) else 130
        return "interrupted", code
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
    code = error.code if isinstance(error, SystemExit) and isinstance(error.code, int) else 1
    return "failed", code


def interactive_shell(
    connection: tuple[Any, dict[str, Any]],
    log: RunLog,
    *,
    reconnect: Callable[[tuple[Any, dict[str, Any]]], tuple[Any, dict[str, Any]]],
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
                connection = reconnect(connection)
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
    loader_trace: bool,
    inspect_card: CardInspector | None = None,
    install_card: CardInstaller | None = None,
) -> None:
    bundle, runtime = admitted.root, admitted.runtime
    log.message(f"FPLinux — {runtime['identity']['target']['display_name']}")
    system_card = boots_system_card(runtime)
    spec = phone_data_runtime.device_data_spec(bundle, runtime)
    selected = None
    card_action = inspect_card is not None or install_card is not None
    firmware = {}
    if spec is not None:
        from fplinux_cli.device_data.store import locked_phone_store  # noqa: PLC0415

        with locked_phone_store(runtime["target"], root=phone_data) as store:
            if spec.required_declarations or prepare_only or from_dump is not None or card_action:
                reader_used = phone_data_runtime.prepare_phone_data(
                    admitted,
                    data,
                    log,
                    emit,
                    store=store,
                    from_dump=from_dump,
                    replace=replace,
                    loader_trace=loader_trace,
                    retain_reader=card_action,
                )
                if not card_action and (prepare_only or reader_used):
                    return
            if not card_action:
                selected = phone_data_runtime.selected_phone_data(spec, store / "phone")
            elif install_card is not None:
                from fplinux_cli.device_data.inputs import (  # noqa: PLC0415 -- card only.
                    capture_phone_data,
                )

                firmware = capture_phone_data(spec.groups, store / "phone", require_all=False)
    elif prepare_only or from_dump is not None or card_action:
        fail("This bundle boots directly and does not require phone-data preparation")
    if card_action:
        with log.capture():
            reader_connection = boot.reconnect_live_bundle(
                admitted,
                session_root=data / "reader/session",
                prepared_data_sha256=phone_data_runtime.content_identity(
                    phone_data_runtime.capsule_bytes({}, preparation=True)
                ),
                required=True,
            )
        if reader_connection is None:
            fail("The phone left its RAM preparation session")
        if inspect_card is not None:
            inspect_card(reader_connection, log)
        elif install_card is not None:
            install_card(reader_connection, firmware, log)
            phone_data_runtime.finish_reader(
                reader_connection, data, log, progress=None, restart=False
            )
            log.message("System card verified. Power off the phone before starting from the card.")
        return
    if not system_card and (data / "reader").exists():
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
    contents = None if system_card else phone_data_runtime.selected_capsule(selected)
    identity = phone_data_runtime.content_identity(contents)
    log.message("Checking the saved phone connection...")
    with log.capture():
        connection = boot.reconnect_live_bundle(
            admitted,
            session_root=data / "session",
            prepared_data_sha256=identity,
        )
    if connection is None:
        log.message("Starting FPLinux from microSD..." if system_card else "Starting FPLinux...")
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
    if not os.isatty(0):
        log.message("No interactive terminal is attached; the loader is complete.")
    else:
        interactive_shell(
            connection,
            log,
            reconnect=partial(
                boot.reconnect_saved_connection, admitted, prepared_data_sha256=identity
            ),
        )


def run_phone(  # noqa: PLR0913 -- explicit CLI policies keep preparation and remote actions distinct.
    bundle: Path,
    data_root: Path,
    log_root: Path,
    *,
    phone_data: Path | None = None,
    from_dump: Path | None = None,
    prepare_only: bool = False,
    replace: bool = False,
    events: Path | None = None,
    loader_trace: bool = False,
    nand_action: str | None = None,
    nand_output: Path | None = None,
    inspect_card: CardInspector | None = None,
    install_card: CardInstaller | None = None,
) -> int:
    """Run the shared workflow with diagnostics and one state lock through SSH exit."""
    log = None
    status = "failed"
    try:
        data = private_directory(data_root)
        remote_action = nand_action is not None
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
                        loader_trace=loader_trace,
                        inspect_card=inspect_card,
                        install_card=install_card,
                    )
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

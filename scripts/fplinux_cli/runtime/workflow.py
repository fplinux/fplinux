# SPDX-License-Identifier: GPL-2.0-only
"""Own preparation, reconnect, launch diagnostics and the interactive SSH lifetime."""

from __future__ import annotations

import hashlib
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import traceback
from contextlib import contextmanager, nullcontext, suppress
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import replace_file_atomically

from . import boot
from .bundle import admit_bundle, fail, host_preflight
from .connection import ConnectionLostError
from .events import RunLog, record_events
from .state import exclusive_lock, private_directory

if TYPE_CHECKING:
    from collections.abc import Callable, Iterator
    from types import FrameType

    from fplinux_cli.device_data.inputs import FirmwareInput
    from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools

    from .bundle import ValidatedBundle

CLOCK_STATUS = re.compile(r"system=(?P<system>[0-9]+) rtc=(?P<rtc>kept|written|failed)")


def _protect_private(path: Path) -> None:
    private_directory(path)


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


def _device_data_spec(bundle: Path, runtime: dict[str, Any]) -> DeviceDataSpec | None:
    descriptor = runtime["device_data"]
    if descriptor is None:
        return None
    from fplinux_cli.device_data.formats import AudioProfileContext  # noqa: PLC0415
    from fplinux_cli.device_data.prepare import DeviceDataSpec  # noqa: PLC0415

    parser = descriptor["parser"]
    originals_parser = descriptor["originals_parser"]
    ecc = descriptor["ecc_module"]
    return DeviceDataSpec(
        target=runtime["target"],
        parser=bundle / parser if parser is not None else None,
        originals_parser=bundle / originals_parser if originals_parser is not None else None,
        groups=descriptor["groups"],
        required_groups=tuple(descriptor["required_groups"]),
        audio=AudioProfileContext(
            runtime["identity"]["target"]["compatible"].encode(), **descriptor["audio_policy"]
        ),
        preparation_recipe=descriptor["preparation_recipe"],
        nand=runtime["nand"],
        emmc=runtime["emmc"],
        ecc_module=bundle / ecc if ecc is not None else None,
    )


def _phone_data_tools(admitted: ValidatedBundle) -> DeviceDataTools:
    """Resolve delivered tools for the same preparation and reconnect decisions."""
    from fplinux_cli.device_data.prepare import DeviceDataTools  # noqa: PLC0415

    tool = admitted.runtime["host_tools"].get("nand_ecc")
    return DeviceDataTools(
        host_tools=(admitted.root / tool).parent if tool is not None else None,
        protect_private=_protect_private,
        python_modules=admitted.root / "scripts" if admitted.runtime["emmc"] is not None else None,
    )


def _selected_phone_data(
    spec: DeviceDataSpec, destination: Path
) -> dict[str, tuple[FirmwareInput, ...]]:
    """Require normal-boot groups and retain any separately imported optional firmware."""
    from fplinux_cli.device_data.inputs import capture_phone_data  # noqa: PLC0415

    captured = capture_phone_data(spec.groups, destination, require_all=False)
    missing = set(spec.required_declarations) - captured.keys()
    if missing:
        fail(f"Required phone data is absent: {', '.join(sorted(missing))}")
    return captured


def _capsule_bytes(
    selected: dict[str, tuple[FirmwareInput, ...]], *, preparation: bool = False
) -> bytes:
    from fplinux_cli.device_data.cpio import create_device_data_initramfs  # noqa: PLC0415

    return create_device_data_initramfs(
        selected, usb_transport="rndis" if os.name == "nt" else None, preparation=preparation
    )


def _content_identity(contents: bytes | None) -> str | None:
    return hashlib.sha256(contents).hexdigest() if contents is not None else None


def _selected_capsule(
    selected: dict[str, tuple[FirmwareInput, ...]] | None,
) -> bytes | None:
    """Supply firmware or the native Windows transport choice for this boot."""
    if selected is not None:
        return _capsule_bytes(selected)
    if os.name == "nt":
        return _capsule_bytes({})
    return None


def prepared_content_identity(
    bundle: Path, runtime: dict[str, Any], *, phone_data: Path | None = None
) -> str | None:
    """Bind reconnect operations to the admitted bytes selected for a normal RAM boot."""
    spec = _device_data_spec(bundle, runtime)
    selected = None
    if spec is not None:
        from fplinux_cli.device_data.store import locked_phone_store  # noqa: PLC0415

        with locked_phone_store(runtime["target"], root=phone_data) as store:
            selected = _selected_phone_data(spec, store / "phone")
    return _content_identity(_selected_capsule(selected))


def _boot_connection(  # noqa: PLR0913 -- loaded content and owned session/log roots stay explicit.
    admitted: ValidatedBundle,
    session_root: Path,
    log: RunLog,
    emit: Callable[[str], None],
    *,
    capsule: Path | None,
    prepared_data_sha256: str | None,
    loader_trace: bool,
) -> tuple[Any, dict[str, Any]]:
    resumed = False

    def progress(stage: str) -> None:
        nonlocal resumed
        if stage == "session-resumed":
            resumed = True
        emit(stage)

    with log.capture() as output:
        connection = boot.run_bundle(
            admitted,
            progress,
            device_data=capsule,
            prepared_data_sha256=prepared_data_sha256,
            output=output,
            session_root=session_root,
            loader_trace=loader_trace,
            log=log.path,
        )
    if not resumed:
        _sync_clock(connection, log)
    return connection


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


def _sync_clock(connection: tuple[Any, dict[str, Any]], log: RunLog) -> None:
    """Set fresh RAM sessions from host UTC and report the phone's clock outcome."""
    transport, session = connection
    host_seconds = int(time.time())
    with log.capture():
        result = transport.run_remote(
            session, f"fplinux-clock {host_seconds}", capture_output=True, shared=True
        )
    status = CLOCK_STATUS.fullmatch(result.stdout.strip())
    detail = result.stderr.strip().splitlines()
    reason = detail[-1] if detail else f"unexpected clock status (exit {result.returncode})"
    if status is None:
        log.message(f"fplinux ssh: phone clock was not set: {reason}")
        return
    phone_time = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(int(status["system"])))
    if status["rtc"] == "failed":
        log.message(
            f"fplinux ssh: phone clock set to {phone_time}, but its RTC was not written: {reason}"
        )
        return
    log.message(f"Phone clock set to {phone_time} (RTC {status['rtc']}).")


def _reader_connection(
    admitted: ValidatedBundle,
    data: Path,
    log: RunLog,
    emit: Callable[[str], None],
    *,
    loader_trace: bool,
) -> tuple[Any, dict[str, Any]]:
    reader = private_directory(data / "reader")
    contents = _capsule_bytes({}, preparation=True)
    identity = _content_identity(contents)
    with log.capture():
        connection = boot.reconnect_live_bundle(
            admitted, session_root=reader / "session", prepared_data_sha256=identity
        )
    if connection is not None:
        log.message("Continuing the saved read-only preparation session...")
        return connection
    capsule = reader / "device-data.cpio"
    replace_file_atomically(capsule, contents, 0o600)
    log.message("First run: keep the phone powered off and disconnected.")
    return _boot_connection(
        admitted,
        reader / "session",
        log,
        emit,
        capsule=capsule,
        prepared_data_sha256=identity,
        loader_trace=loader_trace,
    )


class _PreparationProgress:
    """Publish optional phone feedback without changing backup or preparation results."""

    def __init__(self, connection: tuple[Any, dict[str, Any]], log: RunLog) -> None:
        self.connection = connection
        self.log = log
        self.enabled = True

    def stage(self, stage: str) -> None:
        """Publish a preparation stage without inventing a completion estimate."""
        self._publish(["--stage", stage])

    def reading(self, completed: int, total: int) -> None:
        """Publish the same measured byte counts shown by the host's NAND progress."""
        self._publish(["--stage", "reading", "--completed", str(completed), "--total", str(total)])

    def _publish(self, options: list[str]) -> None:
        if not self.enabled:
            return
        transport, session = self.connection
        command = shlex.join(
            [
                "/usr/libexec/fplinux/preparation",
                *options,
                "/run/fplinux-preparation-status",
            ]
        )
        try:
            with self.log.capture():
                result = transport.run_remote(session, command, capture_output=True, shared=True)
        except (OSError, ValueError, SystemExit, subprocess.SubprocessError) as error:
            if isinstance(error, SystemExit) and error.code in (129, 130, 143):
                raise
            reason = str(error)
        else:
            if result.returncode == 0:
                return
            reason = result.stderr.strip() or f"exit {result.returncode}"
        self.enabled = False
        self.log.message(
            "Warning: phone preparation progress is unavailable; continuing on the computer."
        )
        self.log.record(f"Phone preparation progress: {reason}")


@contextmanager
def _preparation_progress(
    connection: tuple[Any, dict[str, Any]] | None, log: RunLog
) -> Iterator[_PreparationProgress | None]:
    """Retain reader access and the original failure when preparation is interrupted."""
    progress = _PreparationProgress(connection, log) if connection is not None else None
    try:
        if progress is not None:
            progress.stage("waiting")
        yield progress
    except BaseException as error:
        if progress is not None:
            cancelled = isinstance(error, KeyboardInterrupt) or (
                isinstance(error, SystemExit) and error.code in (129, 130, 143)
            )
            # A failed notification must not replace the operation's original exception.
            with suppress(BaseException):
                progress.stage("cancelled" if cancelled else "error")
        raise


def _read_backup(  # noqa: PLR0913 -- NAND output and optional phone feedback have distinct owners.
    connection: tuple[Any, dict[str, Any]],
    runtime: dict[str, Any],
    data: Path,
    log: RunLog,
    *,
    output: Path | None = None,
    phone_progress: _PreparationProgress | None = None,
) -> Path:
    from .nand_backup import backup_nand  # noqa: PLC0415

    nand = runtime["nand"]
    if not isinstance(nand, dict):
        fail("This bundle does not declare a read-only NAND reader")
    if not isinstance(nand.get("raw_device"), str):
        fail("This bundle does not declare its read-only NAND reader")
    started = time.monotonic()
    last_update = 0.0

    def progress(completed: int, total: int) -> None:
        nonlocal last_update
        now = time.monotonic()
        if completed not in (0, total) and now - last_update < 1:
            return
        last_update = now
        mib = 1024 * 1024
        elapsed = max(now - started, 0.001)
        log.message(
            f"Reading NAND: {completed / total:6.1%} "
            f"({completed / mib:.1f}/{total / mib:.1f} MiB) "
            f"· {completed / mib / elapsed:.2f} MiB/s"
        )
        if phone_progress is not None:
            phone_progress.reading(completed, total)

    log.message("Reading the complete NAND backup; keep USB connected. Ctrl+C cancels.")
    with log.capture():
        backup = backup_nand(
            lambda: connection,
            output or data / "nand.bin",
            target=runtime["target"],
            acquisition_recipe=nand["acquisition_recipe"],
            raw_device=nand["raw_device"],
            declared_id=nand.get("id"),
            declared_raw_page_bytes=nand.get("raw_page_bytes"),
            progress=progress,
        )
    log.message(f"Backup saved: {backup}")
    return backup


def _run_nand(  # noqa: PLR0913 -- the admitted connection, action and selected data stay explicit.
    admitted: ValidatedBundle,
    data: Path,
    log: RunLog,
    *,
    action: str,
    output: Path | None,
    phone_data: Path | None,
) -> None:
    """Use the same saved normal session and content binding for explicit NAND commands."""
    from .nand_backup import read_geometry_report  # noqa: PLC0415

    bundle, runtime = admitted.root, admitted.runtime
    nand = runtime["nand"]
    if not isinstance(nand, dict) or not isinstance(nand.get("raw_device"), str):
        fail("This bundle does not declare a read-only NAND reader")
    with log.capture():
        connection = boot.reconnect_live_bundle(
            admitted,
            session_root=data / "session",
            prepared_data_sha256=prepared_content_identity(bundle, runtime, phone_data=phone_data),
            required=True,
        )
    if connection is None:
        fail("No authenticated phone session is available")
    if action == "backup":
        if output is None:
            fail("NAND backup requires an output path")
        _read_backup(connection, runtime, data, log, output=output)
        return
    transport, session = connection
    with log.capture():
        report = read_geometry_report(transport, session, nand["raw_device"])
    print(report, end="", flush=True)
    log.record(report.rstrip("\n"))


def _finish_reader(
    connection: tuple[Any, dict[str, Any]],
    data: Path,
    log: RunLog,
    *,
    progress: _PreparationProgress | None,
) -> None:
    """Keep recovery keys until read-only inspection proves no persistent storage is in use."""
    transport, session = connection
    with log.capture():
        result = transport.run_remote(
            session,
            "cat /proc/mounts && printf '\\nFPLINUX_SWAPS\\n' && cat /proc/swaps",
            capture_output=True,
            shared=True,
        )
    if result.returncode:
        fail(f"Cannot check storage; reader access is retained in {data / 'reader'}")
    mounts, separator, swaps = result.stdout.partition("\nFPLINUX_SWAPS\n")
    if not separator or not mounts.strip() or not swaps.startswith("Filename"):
        fail(f"Storage inspection was incomplete; reader access is retained in {data / 'reader'}")
    persistent = []
    for line in mounts.splitlines():
        fields = line.split()
        if not fields or not fields[0].startswith("/dev/"):
            continue
        source = fields[0]
        ram_lower = (
            len(fields) >= 4
            and re.fullmatch(r"/dev/loop[0-9]+", source) is not None
            and fields[1] == "/run/fplinux-root/lower"
            and fields[2] == "squashfs"
            and "ro" in fields[3].split(",")
            and "rw" not in fields[3].split(",")
        )
        if not source.startswith("/dev/ram") and not ram_lower:
            persistent.append(source)
    active_swaps = [
        fields[0]
        for line in swaps.splitlines()[1:]
        if (fields := line.split()) and re.fullmatch(r"/dev/zram[0-9]+", fields[0]) is None
    ]
    if persistent or active_swaps:
        fail(
            "Persistent storage is still mounted or used as swap. "
            "Release it before disconnecting; "
            f"reader access is retained in {data / 'reader'}"
        )
    if progress is not None:
        progress.stage("ready")
    shutil.rmtree(data / "reader")
    log.message("Disconnect USB. If the phone stays on, remove and reinsert its battery.")
    log.message("Run the same launcher again with the phone powered off.")


def _prepare_phone_data(  # noqa: PLR0913 -- preparation inputs and their state owners are distinct.
    admitted: ValidatedBundle,
    data: Path,
    log: RunLog,
    emit: Callable[[str], None],
    *,
    store: Path,
    from_dump: Path | None,
    replace: bool,
    loader_trace: bool,
) -> bool:
    """Refresh shared data and report whether a live reader needs a second boot."""
    from fplinux_cli.device_data.cache import (  # noqa: PLC0415 -- selected preparation.
        current_backup,
        prepared_data_current,
    )
    from fplinux_cli.device_data.prepare import (  # noqa: PLC0415
        import_phone_backup,
        prepare_from_nand,
    )

    bundle, runtime = admitted.root, admitted.runtime
    spec = _device_data_spec(bundle, runtime)
    if spec is None:
        fail("This bundle does not require phone-data preparation")
    if spec.emmc is not None:
        return _prepare_emmc_data(
            admitted,
            data,
            log,
            emit,
            store=store,
            from_dump=from_dump,
            replace=replace,
            loader_trace=loader_trace,
        )
    tools = _phone_data_tools(admitted)
    connection = None
    backup = store / "nand.bin"
    if from_dump is not None:
        import_phone_backup(
            spec,
            tools,
            from_dump,
            backup,
            acquisition_recipe=runtime["nand"]["acquisition_recipe"],
        )
    record = current_backup(backup, target=runtime["target"], nand=runtime["nand"])
    if record is None:
        log.message("No compatible NAND backup is available; a new backup is required.")
        connection = _reader_connection(admitted, data, log, emit, loader_trace=loader_trace)
    elif (data / "reader").is_dir():
        with log.capture():
            connection = boot.reconnect_live_bundle(
                admitted,
                session_root=data / "reader/session",
                prepared_data_sha256=_content_identity(_capsule_bytes({}, preparation=True)),
            )
    with _preparation_progress(connection, log) as progress:
        if record is None and connection is not None:
            _read_backup(connection, runtime, data, log, output=backup, phone_progress=progress)
            record = current_backup(backup, target=runtime["target"], nand=runtime["nand"])
        if record is None:
            fail("The NAND backup did not satisfy the selected reader contract")
        destination = store / "phone"
        if replace or not prepared_data_current(spec, tools, destination, record):
            log.message("Preparing your phone data from the saved NAND backup...")
            if progress is not None:
                progress.stage("extracting")
            with log.capture():
                prepare_from_nand(spec, backup, destination, tools, replace=destination.exists())
        else:
            log.message("Using your saved phone data.")
        log.message(f"Your phone data is ready in {destination}.")
        if connection is not None:
            _finish_reader(connection, data, log, progress=progress)
        elif (data / "reader").is_dir():
            shutil.rmtree(data / "reader")
    return connection is not None


def _prepare_emmc_data(  # noqa: PLR0913 -- selected source and reader lifetime remain explicit.
    admitted: ValidatedBundle,
    data: Path,
    log: RunLog,
    emit: Callable[[str], None],
    *,
    store: Path,
    from_dump: Path | None,
    replace: bool,
    loader_trace: bool,
) -> bool:
    """Capture selected stock files once and reprepare them without another full disk dump."""
    from fplinux_cli.device_data.cache import (  # noqa: PLC0415
        current_original_files,
        prepared_originals_current,
    )
    from fplinux_cli.device_data.emmc import (  # noqa: PLC0415
        extract_files,
        validate_file_selections,
    )
    from fplinux_cli.device_data.original_files import save_original_files  # noqa: PLC0415
    from fplinux_cli.device_data.prepare import prepare_from_originals  # noqa: PLC0415

    from .emmc_files import read_originals  # noqa: PLC0415

    spec = _device_data_spec(admitted.root, admitted.runtime)
    if spec is None or spec.emmc is None:
        fail("This bundle does not declare eMMC preparation")
    tools = _phone_data_tools(admitted)
    selected = validate_file_selections(spec.emmc["files"])
    originals = store / "originals"
    connection = None
    if from_dump is not None:
        log.message("Reading selected phone files from the eMMC backup...")
        if tools.python_modules is None:
            fail("This bundle lacks its eMMC filesystem reader")
        with log.capture():
            files = extract_files(from_dump, selected, python_modules=tools.python_modules)
            save_original_files(
                spec.target,
                files,
                originals,
                acquisition_recipe=spec.emmc["acquisition_recipe"],
            )
    source = current_original_files(spec, originals)
    if source is None:
        log.message("The phone's saved settings are not available; reading them from eMMC.")
        connection = _reader_connection(admitted, data, log, emit, loader_trace=loader_trace)
    elif (data / "reader").is_dir():
        with log.capture():
            connection = boot.reconnect_live_bundle(
                admitted,
                session_root=data / "reader/session",
                prepared_data_sha256=_content_identity(_capsule_bytes({}, preparation=True)),
            )
    with _preparation_progress(connection, log) as progress:
        if source is None and connection is not None:
            transport, session = connection
            log.message("Reading the phone's sound settings; keep USB connected. Ctrl+C cancels.")
            with log.capture():
                files = read_originals(transport, session, spec.emmc["raw_device"], selected)
                save_original_files(
                    spec.target,
                    files,
                    originals,
                    acquisition_recipe=spec.emmc["acquisition_recipe"],
                )
            source = current_original_files(spec, originals)
        if source is None:
            fail("The saved phone files did not satisfy the selected acquisition contract")
        destination = store / "phone"
        if replace or not prepared_originals_current(spec, tools, destination, source):
            log.message("Preparing your phone data from the saved settings...")
            if progress is not None:
                progress.stage("extracting")
            with log.capture():
                prepare_from_originals(
                    spec,
                    originals / "manifest.json",
                    destination,
                    tools,
                    replace=destination.exists(),
                )
        else:
            log.message("Using your saved phone data.")
        log.message(f"Your phone data is ready in {destination}.")
        if connection is not None:
            _finish_reader(connection, data, log, progress=progress)
        elif (data / "reader").is_dir():
            shutil.rmtree(data / "reader")
    return connection is not None


def _phone_preparation_current(
    admitted: ValidatedBundle, spec: DeviceDataSpec, store: Path
) -> bool:
    """Use the acquisition contract matching the selected medium before reconnecting."""
    from fplinux_cli.device_data.cache import (  # noqa: PLC0415
        current_backup,
        current_original_files,
        prepared_data_current,
        prepared_originals_current,
    )

    tools = _phone_data_tools(admitted)
    if spec.emmc is not None:
        source = current_original_files(spec, store / "originals")
        return source is not None and prepared_originals_current(
            spec, tools, store / "phone", source
        )
    record = current_backup(store / "nand.bin", target=spec.target, nand=admitted.runtime["nand"])
    return record is not None and prepared_data_current(spec, tools, store / "phone", record)


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
    spec = _device_data_spec(bundle, runtime)
    selected = None
    if spec is not None:
        from fplinux_cli.device_data.store import locked_phone_store  # noqa: PLC0415

        with locked_phone_store(runtime["target"], root=phone_data) as store:
            if reconnect:
                if not _phone_preparation_current(admitted, spec, store):
                    fail("Prepare the phone data before reconnecting")
            else:
                reader_used = _prepare_phone_data(
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
            selected = _selected_phone_data(spec, store / "phone")
    elif prepare_only or from_dump is not None:
        fail("This bundle boots directly and does not require phone-data preparation")
    if not reconnect and (data / "reader").exists():
        with log.capture():
            reader_connection = boot.reconnect_live_bundle(
                admitted,
                session_root=data / "reader/session",
                prepared_data_sha256=_content_identity(_capsule_bytes({}, preparation=True)),
            )
        if reader_connection is not None:
            with _preparation_progress(reader_connection, log) as progress:
                _finish_reader(reader_connection, data, log, progress=progress)
            return
        shutil.rmtree(data / "reader")
    contents = _selected_capsule(selected)
    identity = _content_identity(contents)
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
            connection = _boot_connection(
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
                protect=_protect_private,
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
                    _run_nand(
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

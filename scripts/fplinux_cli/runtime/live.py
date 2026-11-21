# SPDX-License-Identifier: GPL-2.0-only
"""Select and authenticate an existing phone without starting a new boot."""

from __future__ import annotations

import os
import subprocess
import sys
from contextlib import nullcontext, suppress
from dataclasses import dataclass
from enum import StrEnum
from typing import TYPE_CHECKING, Any

from . import boot
from . import phone_data as phone_data_runtime
from .bundle import admit_bundle, boots_system_card, fail, host_preflight, load_runtime_manifest
from .card_access import enrolled_endpoints
from .connection import CardBootEntry, RunningBuild, read_running_build, read_running_card
from .events import RunLog
from .operations import BundlePaths, LiveRequest, SelectedConnection, file_transfer_available
from .screenshot import capture_screenshot
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
    from .operations import FileTransfer


def load_connections(  # noqa: PLR0913 -- fixed bundles and archive projections admit host files separately.
    target: str,
    session_root: Path,
    *,
    bundle_root: Path | None = None,
    runtime: dict[str, Any] | None = None,
    host_file: Callable[[str], Path] | None = None,
    shared_root: bool = False,
) -> list[tuple[Any, dict[str, Any]]]:
    """Read saved credentials from this installation without creating a session."""
    if not session_root.exists():
        return []
    if os.name == "nt":
        from .windows.bundle import validate_connection_tools  # noqa: PLC0415
        from .windows.host import WindowsHost  # noqa: PLC0415
        from .windows.transport import WindowsTransport  # noqa: PLC0415

        if runtime is None:
            fail("Windows connection requires its archive and runtime metadata")
        if host_file is not None:
            tools = validate_connection_tools(host_file)
        elif bundle_root is not None:
            tools = validate_connection_tools(lambda name: bundle_root / name)
        else:
            fail("Windows connection requires its archive and runtime metadata")
        transport = WindowsTransport(
            WindowsHost(), tools, wait_seconds=runtime["usb"]["linux_gadget"]["wait_seconds"]
        )
        return [
            (transport, session)
            for session in transport.load_saved_sessions(
                target, session_root=session_root, shared_root=shared_root
            )
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


@dataclass(frozen=True)
class SelectedCard:
    """An exact card endpoint or a launcher connection to check against enrolled card pins."""

    connection: tuple[Any, dict[str, Any]]
    cid: str
    entry: CardBootEntry
    pins: dict[str, str]


def _select_card(
    target: str,
    cid: str,
    session_root: Path,
    bundle: Path | None,
    runtime: dict[str, Any] | None,
) -> SelectedCard:
    windows = None
    transport: Any = ssh_transport
    if os.name == "nt":
        from .windows.bundle import validate_connection_tools  # noqa: PLC0415
        from .windows.host import WindowsHost  # noqa: PLC0415
        from .windows.transport import WindowsTransport  # noqa: PLC0415

        if bundle is None or runtime is None:
            fail("Windows card access requires its archive and runtime metadata")
        tools = validate_connection_tools(lambda name: bundle / name)
        windows = WindowsHost()
        transport = WindowsTransport(
            windows, tools, wait_seconds=runtime["usb"]["linux_gadget"]["wait_seconds"]
        )
    profiles = enrolled_endpoints(target, cid, windows)
    attached = [profile for profile in profiles if transport.usb_present(profile)]
    if len(attached) > 1:
        fail("More than one USB device matches the selected microSD enrollment")
    if attached:
        endpoint = attached[0]
        return SelectedCard(
            (transport, endpoint),
            cid,
            CardBootEntry.SD,
            {endpoint["usb_serial"]: endpoint["host_key"]},
        )

    # USB-assisted startup uses the transient launcher binding, not the card keys.
    if not session_root.is_dir():
        sessions = []
    elif windows is not None:
        sessions = transport.load_saved_sessions(
            target, session_root=session_root, shared_root=True
        )
    else:
        session = ssh_session.load_saved_session(target, runtime_root=session_root)
        sessions = [session] if session is not None else []
    connected = [session for session in sessions if transport.usb_present(session)]
    if not connected:
        fail(f"No connected session for enrolled microSD {cid}; connect the running phone")
    if len(connected) > 1:
        fail("More than one saved USB-assisted phone session is connected")
    pins = {profile["usb_serial"]: profile["host_key"] for profile in profiles}
    return SelectedCard((transport, connected[0]), cid, CardBootEntry.USB, pins)


def _checked_card_build(
    card: SelectedCard, connection: tuple[Any, dict[str, Any]], admitted: ValidatedBundle | None
) -> RunningBuild:
    observed = read_running_card(connection)
    if observed.entry != card.entry or observed.target != connection[1]["target"]:
        fail("The running phone uses a different boot entry or microSD target")
    if observed.cid != card.cid:
        fail("The running SD card CID does not match --card")
    if observed.serial not in card.pins or observed.host_key != card.pins[observed.serial]:
        fail("The running microSD does not match the saved card SSH enrollment")
    if admitted is not None:
        if observed.device_identity != admitted.device_identity:
            fail("The running microSD has a different kernel identity or boot recipe")
        if observed.build.build_id != admitted.runtime["release_id"]:
            fail("The running microSD root has a different build identity")
    return observed.build


def _prepared_identity(admitted: ValidatedBundle, phone_data: Path | None) -> str | None:
    if boots_system_card(admitted.runtime):
        return None
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


def _show_running_build(
    target: str,
    observed: RunningBuild,
    log: RunLog,
) -> None:
    slot = f"; microSD slot: {observed.card_slot}" if observed.card_slot is not None else ""
    log.message(
        f"Phone: {target}; kernel: {observed.kernel_release}; BUILD_ID: {observed.build_id}{slot}"
    )


class _CardMountpoint(StrEnum):
    SLOT_ROOT = "/run/fplinux-card"
    DIRECT_ROOT = "/"


def _shutdown_mountpoint(admitted: ValidatedBundle | None) -> _CardMountpoint:
    if admitted is not None:
        if boots_system_card(admitted.runtime):
            return _CardMountpoint.SLOT_ROOT
        if admitted.runtime.get("profile") == "microsd-root":
            return _CardMountpoint.DIRECT_ROOT
    return fail("Shutdown requires a selected microSD system build")


def _decode_readonly_mountpoint(output: str) -> _CardMountpoint | None:
    status, separator, mountpoint = output.strip().partition(" ")
    if status == "readonly" and separator:
        with suppress(ValueError):
            return _CardMountpoint(mountpoint)
    return None


def _shutdown_card(
    connection: tuple[Any, dict[str, Any]], log: RunLog, *, mountpoint: _CardMountpoint
) -> None:
    """Require a complete read-only receipt before sending a separate power-off request."""
    transport, session = connection
    helper = "/usr/libexec/fplinux/microsd-grow"
    log.message("Preparing system-card shutdown. Keep USB connected.")
    prepared = transport.run_remote(
        session, f"{helper} prepare-poweroff", capture_output=True, shared=True, timeout=60
    )
    detail = prepared.stderr.strip()
    if detail:
        log.record(detail)
    if prepared.returncode:
        fail(detail or f"Could not prepare system-card shutdown (exit {prepared.returncode})")
    if _decode_readonly_mountpoint(prepared.stdout) != mountpoint:
        fail("The phone did not confirm a synchronized, read-only system card")
    log.message(f"System card synchronized and confirmed read-only at {mountpoint}.")

    log.message("Requesting power-off. Keep USB connected until the phone has shut down.")
    try:
        requested = transport.run_remote(
            session, f"{helper} poweroff", capture_output=True, shared=True, timeout=15
        )
    except subprocess.TimeoutExpired as error:
        message = "SSH timed out before confirming the power-off request"
        raise SystemExit(message) from error
    detail = requested.stderr.strip()
    if detail:
        log.record(detail)
    if requested.returncode == 0:
        log.message("The phone accepted the power-off request.")
    elif requested.returncode != 255:
        fail(detail or f"The phone refused the power-off request (exit {requested.returncode})")

    if transport.usb_present(session):
        log.message("The phone's USB connection is still detected.")
    else:
        log.message("The phone's USB connection is no longer detected.")
    if requested.returncode != 0:
        fail("SSH could not confirm the power-off request")
    log.message("Check that the phone has shut down before removing power or the card.")


def console_connection(  # noqa: PLR0913 -- selected session and action policies stay explicit.
    connection: tuple[Any, dict[str, Any]],
    data_root: Path,
    log_root: Path,
    *,
    admitted: ValidatedBundle | None,
    phone_data: Path | None = None,
    exec_command: str | None = None,
    transfer: FileTransfer | None = None,
    screenshot: Path | None = None,
    verify_only: bool = False,
    shutdown: bool = False,
    revalidate: Callable[[], None] | None = None,
    log: RunLog | None = None,
    card: SelectedCard | None = None,
) -> int:
    """Authenticate one selected session and perform at most one remote action."""
    status = "failed"
    try:
        private_directory(data_root)
        remote_action = any(value is not None for value in (exec_command, transfer, screenshot))
        lock = (
            nullcontext()
            if verify_only or remote_action
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
            if verify_only and (admitted is None or shutdown or remote_action):
                fail("Verification requires a selected build and no remote action")
            if shutdown and (remote_action or phone_data is not None):
                fail("Shutdown requires a selected microSD system and no other console action")
            mountpoint = _shutdown_mountpoint(admitted) if shutdown else None
            if transfer is not None and not file_transfer_available():
                fail("File transfer is available from a Linux host")
            if admitted is None:
                log.message("Diagnostic connection: the current build is not being verified.")
            identity: str | None = None
            identity_captured = False
            card_build: RunningBuild | None = None

            def reconnect(current: tuple[Any, dict[str, Any]]) -> tuple[Any, dict[str, Any]]:
                nonlocal identity, identity_captured, card_build
                if revalidate is not None:
                    revalidate()
                if card is not None and card.entry == "sd":
                    card_build = _checked_card_build(card, current, admitted)
                    return current
                if admitted is not None:
                    if current[1]["target"] != admitted.runtime["target"]:
                        fail("The selected build belongs to another phone target")
                    current_identity = _prepared_identity(admitted, phone_data)
                    if identity_captured and current_identity != identity:
                        fail("The selected phone data changed while the console was open")
                    identity, identity_captured = current_identity, True
                    current = boot.reconnect_saved_connection(
                        admitted, current, prepared_data_sha256=identity
                    )
                else:
                    transport, session = current
                    current = (
                        transport,
                        transport.reacquire_bound_session(session, expected_device_identity=None),
                    )
                if card is not None:
                    card_build = _checked_card_build(card, current, admitted)
                return current

            if card is not None:
                with log.capture():
                    connection = reconnect(connection)
            else:
                transport, session = connection
                with log.capture():
                    session = transport.reacquire_bound_session(
                        session, expected_device_identity=None
                    )
                connection = transport, session
                if admitted is not None:
                    with log.capture():
                        connection = reconnect(connection)
            transport, session = connection
            inspect_card = (
                (verify_only or shutdown)
                and admitted is not None
                and boots_system_card(admitted.runtime)
            )
            expected_build = (
                admitted.runtime["release_id"]
                if (inspect_card or shutdown) and admitted is not None
                else None
            )
            observed = card_build or read_running_build(connection, inspect_card=inspect_card)
            if expected_build is not None and observed.build_id != expected_build:
                fail("The running microSD root has a different build identity")
            _show_running_build(str(session["target"]), observed, log)
            if verify_only:
                log.message("The running phone matches the selected build.")
            elif mountpoint is not None:
                _shutdown_card(connection, log, mountpoint=mountpoint)
            elif exec_command is not None:
                result = transport.run_remote(session, exec_command)
                if result.returncode:
                    raise SystemExit(result.returncode)  # noqa: TRY301 -- preserve remote status.
            elif transfer is not None:
                if transfer.direction == "upload":
                    transport.upload(session, str(transfer.local), transfer.remote)
                else:
                    transport.pull(session, transfer.remote, str(transfer.local))
            elif screenshot is not None:
                capture_screenshot(transport, session, screenshot)
                log.message(f"Screenshot saved: {screenshot}")
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


def console_card(request: LiveRequest, paths: BundlePaths | SelectedConnection) -> int:
    """Use one enrolled card through the same runtime from the checkout or a release."""
    target, cid = request.target, request.card_cid
    revalidate = None
    bundle = None
    runtime = None
    try:
        with cancellation_signals():
            if target is None or cid is None:
                fail("Select a TARGET and an enrolled microSD CID")
            if isinstance(paths, SelectedConnection):
                admitted = paths.admitted
                revalidate = paths.revalidate
                if admitted is not None:
                    bundle, runtime = admitted.root, admitted.runtime
            else:
                host_preflight()
                bundle = paths.bundle
                runtime = load_runtime_manifest(bundle / "runtime-manifest.json")
                admitted = None if request.allow_other_build else admit_bundle(bundle)
            if runtime is not None and (
                runtime["target"] != target or not boots_system_card(runtime)
            ):
                fail("The selected build does not match the target's system microSD profile")
            card = _select_card(target, cid, paths.data / "session", bundle, runtime)
    except (
        KeyboardInterrupt,
        OSError,
        ValueError,
        SystemExit,
        subprocess.TimeoutExpired,
    ) as error:
        _, code = report_runtime_error(error, None)
        return code
    return console_connection(
        card.connection,
        paths.data,
        paths.logs,
        admitted=admitted,
        revalidate=revalidate,
        exec_command=request.exec_command,
        transfer=request.transfer,
        screenshot=request.screenshot,
        verify_only=request.verify_only,
        shutdown=request.shutdown,
        card=card,
    )


def console_bundle(  # noqa: PLR0913 -- archive selectors and remote actions are independent.
    bundle: Path,
    data_root: Path,
    log_root: Path,
    *,
    allow_other_build: bool = False,
    exec_command: str | None = None,
    transfer: FileTransfer | None = None,
    screenshot: Path | None = None,
    phone_data: Path | None = None,
    verify_only: bool = False,
    shutdown: bool = False,
    connection: tuple[Any, dict[str, Any]] | None = None,
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
            if connection is None:
                connection = select_connection(
                    load_connections(
                        runtime["target"],
                        data_root / "session",
                        bundle_root=bundle,
                        runtime=runtime,
                    )
                )
            elif connection[1]["target"] != runtime["target"]:
                fail("The selected build belongs to another phone target")
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
        transfer=transfer,
        screenshot=screenshot,
        verify_only=verify_only,
        shutdown=shutdown,
        log=log,
    )

# SPDX-License-Identifier: GPL-2.0-only
"""Boot an admitted bundle or authenticate its existing USB session."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import ssh_transport
from .bundle import adapter_path, fail, host_preflight, load_adapter

if TYPE_CHECKING:
    from collections.abc import Callable
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


def run_bundle(  # noqa: PLR0913 -- boot inputs and their resource destinations are distinct.
    admitted: ValidatedBundle,
    emit: Callable[[str], None],
    *,
    device_data: Path | None = None,
    prepared_data_sha256: str | None = None,
    output: TextIO | None = None,
    session_root: Path,
    loader_trace: bool = False,
    log: Path | None = None,
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


def reconnect_live_bundle(
    admitted: ValidatedBundle,
    *,
    session_root: Path,
    prepared_data_sha256: str | None = None,
    required: bool = False,
) -> tuple[Any, dict[str, Any]] | None:
    """Only proven USB absence permits a fresh boot; mismatches stop the launch."""
    host_preflight()
    runtime = admitted.runtime
    if os.name == "nt":
        from .windows_host import load_live_session  # noqa: PLC0415

        if admitted.windows is None:
            fail("This bundle has no admitted Windows host tools")
        connection = load_live_session(
            admitted.windows,
            session_root=session_root,
            prepared_data_sha256=prepared_data_sha256,
        )
        if connection is None and required:
            fail("the saved phone is absent; start the powered-off phone with the launcher")
        return connection
    session: dict[str, Any] | None
    if required:
        session = ssh_transport.load_current_session(runtime["target"], runtime_root=session_root)
    else:
        session = ssh_transport.load_live_session(runtime["target"], runtime_root=session_root)
    if session is None:
        return None
    ssh_transport.require_prepared_content(
        session,
        bundle_generation=admitted.generation,
        prepared_data_sha256=prepared_data_sha256,
    )
    session = ssh_transport.reacquire_bound_session(session)
    ssh_transport.require_device_identity(session, admitted.device_identity)
    return ssh_transport, session

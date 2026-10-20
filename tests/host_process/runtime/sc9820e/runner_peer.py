# SPDX-License-Identifier: GPL-2.0-only
"""Run the real standalone adapter with local USB and SSH process boundaries."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import signal
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

from fplinux_cli.runtime import ssh_transport

from tests import ROOT
from tests.host_process.runtime.sc9820e.fixtures import load_program, prepare_runtime

if TYPE_CHECKING:
    from collections.abc import Callable
    from types import FrameType


def _stop(signum: int, _frame: FrameType | None) -> None:
    raise SystemExit(128 + signum)


def main() -> None:
    root = Path(sys.argv[1])
    mode = sys.argv[2]
    os.environ["SC9820E_TEST_ROOT"] = str(root)
    os.environ["SC9820E_TEST_MODE"] = mode
    adapter = load_program("sc9820e_process_adapter", ROOT / "platforms/sc9820e/host/adapter.py")
    runtime, session, _assets = prepare_runtime(root)
    runtime_root = root / "runtime"
    name = hashlib.sha256(f"test-phone\0{session['usb_serial']}".encode("ascii")).hexdigest()[:32]
    session_directory = runtime_root / "sessions" / name
    session_directory.mkdir(parents=True, mode=0o700)
    image = session_directory / "ramboot.bin"
    Path(session["image"]).rename(image)
    session.update(
        {
            "image": str(image),
            "target": "test-phone",
            "runtime_root": str(runtime_root),
            "network": "10.23.45.0/30",
            "host_address": "10.23.45.1",
            "phone_address": "10.23.45.2",
            "host_mac": "02:00:00:00:00:01",
            "device_mac": "02:00:00:00:00:02",
            "private_key": str(session_directory / "client_ed25519"),
            "known_hosts": str(session_directory / "known_hosts"),
            "status": "prepared",
        }
    )
    for filename in ("client_ed25519", "known_hosts"):
        path = session_directory / filename
        path.write_text("test-owned", encoding="ascii")
        path.chmod(0o600)
    (session_directory / "owner.json").write_text(
        json.dumps(
            {
                "kind": "fplinux-host-session-owner",
                "target": "test-phone",
                "usb_serial": session["usb_serial"],
            }
        ),
        encoding="utf-8",
    )
    (root / "serial").write_text(session["usb_serial"], encoding="ascii")
    node = root / "nodes/003/041"
    node.parent.mkdir(parents=True)
    node.symlink_to("/dev/null")
    selected = root / "sysfs/3-2.6"
    selected.mkdir(parents=True)
    for filename, value in {
        "idVendor": "1782",
        "idProduct": "4d00",
        "busnum": "3",
        "devnum": "41",
    }.items():
        (selected / filename).write_text(value + "\n", encoding="ascii")
    if mode == "ambiguous":
        shutil.copytree(selected, root / "sysfs/3-2.7")
    loader = root / "loader"
    shutil.copyfile(Path(__file__).with_name("loader_peer.py"), loader)
    loader.chmod(0o700)

    def endpoint(
        prepared: dict[str, Any], *, on_linux_usb: Callable[[], None] | None = None
    ) -> dict[str, Any]:
        if on_linux_usb is not None:
            on_linux_usb()
        return {**prepared, "status": "ready", "interface": "testncm0"}

    def ssh_argv(
        _session: dict[str, Any], command: str | None = None, **_options: object
    ) -> list[str]:
        if command is None:
            message = "the adapter must not open an interactive shell"
            raise AssertionError(message)
        return [sys.executable, str(Path(__file__).with_name("ssh_peer.py")), command]

    def event(name: str) -> None:
        with (root / "events").open("a", encoding="ascii") as stream:
            stream.write(name + "\n")

    signal.signal(signal.SIGTERM, _stop)
    with (
        mock.patch.object(adapter, "USB_DEVICES", root / "sysfs", create=False),
        mock.patch.object(adapter, "USB_NODES", root / "nodes", create=False),
        mock.patch.object(ssh_transport, "wait_for_bound_session", endpoint, create=False),
        mock.patch.object(ssh_transport, "_ssh_argv", ssh_argv, create=False),
    ):
        adapter.run(
            root,
            runtime,
            session,
            expected_device_identity="9" * 64,
            events=event,
            loader_trace=mode == "trace",
        )
    event("complete")


if __name__ == "__main__":
    main()

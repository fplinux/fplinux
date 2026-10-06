# SPDX-License-Identifier: GPL-2.0-only
"""Run the loader lifecycle with controlled bundle, USB and SSH readiness inputs.

The real runner, adapter, event writer, session cleanup and SSH transport execute.
RAM tools and the SSH executable are local doubles; no phone or network is contacted.
"""

from __future__ import annotations

import json
import os
import pty
import shutil
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

from fplinux_cli.runtime import ssh_transport

from common import loader_events
from common import run as runner
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Callable

ROOT = Path(__file__).resolve().parents[3]


def runtime_manifest(transport: str) -> dict[str, Any]:
    """Supply validated bundle inputs without making an artifact-verification claim."""
    return {
        "target": "phone",
        "profile": None,
        "build_type": "release",
        "transport": transport,
        "identity": {
            "target": {
                "brand": "Demo",
                "product": "Phone",
                "hardware_codes": ["D-1"],
                "compatible": "demo,phone",
                "display_name": "Demo Phone (D-1)",
            },
            "platform": {
                "name": "ums9117",
                "vendor": "Unisoc",
                "soc": "UMS9117",
                "aliases": ["T117"],
                "compatible": "sprd,ums9117",
                "display_name": "Unisoc UMS9117",
            },
        },
        "image": "ramboot.bin",
        "personalization": {},
        "assets": {"fdl1": "fdl1.bin"},
        "host_tools": {"loader": "ram-tool", "bridge": "ram-tool", "keyboard": "ram-tool"},
        "adapter": {
            "brightness": 50,
            "rotation": 0,
            "exec_distance": 0,
            "session_name": "test-linux",
            "handoff_wait_seconds": 1,
            "usb_release_wait_seconds": 1,
            "boot_instructions": "Connect the selected phone.",
        },
        "addresses": {"fdl1": 0x6200, "payload": 0x80100000},
        "usb": {
            "bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 1},
            "linux_gadget": {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 1},
        },
        "sha256": {},
    }


def main() -> None:
    """Enter the real runner with a real input terminal when the case requests one."""
    root = Path(sys.argv[1])
    mode = sys.argv[2]
    if mode != "noninteractive":
        _master, terminal = pty.openpty()
        os.dup2(terminal, 0)
        os.close(terminal)
    else:
        with Path(os.devnull).open("rb") as input_stream:
            os.dup2(input_stream.fileno(), 0)
    (root / "runner.pid").write_text(str(os.getpid()), encoding="ascii")
    runtime_root = root / "runtime/fplinux"
    runtime_root.mkdir(parents=True, mode=0o700)
    os.environ["XDG_RUNTIME_DIR"] = str(runtime_root.parent)
    session = create_ready_session(runtime_root, status="prepared")
    Path(session["image"]).write_bytes(b"personalized RAM image")
    (root / "ramboot.bin").write_bytes(b"DHTBpayload")
    (root / "runner").mkdir()
    shutil.copyfile(
        ROOT / "scripts/fplinux_cli/manifests/identity.py", root / "runner/identity.py"
    )
    (root / "ram-tool").write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
    (root / "ram-tool").chmod(0o755)
    tools = root / "bin"
    tools.mkdir()
    # Buffering is irrelevant for the local, silent RAM-tool process double.
    (tools / "stdbuf").write_text('#!/bin/sh\nshift 2\nexec "$@"\n', encoding="ascii")
    (tools / "stdbuf").chmod(0o755)
    ssh_fixture = (ROOT / "tests/fixtures/processes/loader_ssh.py").read_text(encoding="utf-8")
    (tools / "ssh").write_text(f"#!{sys.executable}\n{ssh_fixture}", encoding="utf-8")
    (tools / "ssh").chmod(0o755)
    os.environ["PATH"] = f"{tools}{os.pathsep}{os.environ['PATH']}"
    os.environ["FPLINUX_LOADER_TEST_ROOT"] = str(root)
    os.environ["FPLINUX_LOADER_TEST_MODE"] = mode
    adapter = runner.load_adapter(ROOT / "platforms/ums9117/host/adapter.py")
    manifest = runtime_manifest("none" if mode == "none" else "usb-ncm")

    def wait_for_session(
        prepared: dict[str, Any], *, on_linux_usb: Callable[[], None]
    ) -> dict[str, Any]:
        on_linux_usb()
        if mode == "authentication-failure":
            message = "controlled authentication failure"
            raise SystemExit(message)
        ready = {**prepared, "status": "ready"}
        state = Path(ready["private_key"]).parent / "session.json"
        state.write_text(json.dumps(ready), encoding="utf-8")
        return ready

    with (
        mock.patch.object(runner, "__file__", str(root / "runner/run.py")),
        mock.patch.object(runner, "load_runtime_manifest", return_value=manifest),
        mock.patch.object(runner, "load_adapter", return_value=adapter),
        mock.patch.object(
            runner,
            "load_module",
            side_effect=lambda _path, name: (
                loader_events if name == "fplinux_loader_events" else ssh_transport
            ),
        ),
        mock.patch.dict(sys.modules, {"ssh_transport": ssh_transport}),
        mock.patch.object(ssh_transport, "bundle_identity"),
        mock.patch.object(ssh_transport, "build_manifest_device_identity", return_value="9" * 64),
        mock.patch.object(ssh_transport, "prepare_session", return_value=session),
        mock.patch.object(ssh_transport, "wait_for_bound_session", side_effect=wait_for_session),
        mock.patch.object(adapter, "usb_device_path", return_value=root / "absent-bootrom"),
        mock.patch.object(adapter, "require_usb_device_access"),
        mock.patch.object(sys, "argv", ["run.py", "--events", str(root / "events.jsonl")]),
    ):
        runner.main()


if __name__ == "__main__":
    main()

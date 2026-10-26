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

from fplinux_cli.runtime import boot as runner
from fplinux_cli.runtime import bundle as bundle_runtime
from fplinux_cli.runtime import events as loader_events
from fplinux_cli.runtime import ssh_transport

from tests import ROOT
from tests.ssh_transport_support import create_ready_session

if TYPE_CHECKING:
    from collections.abc import Callable


def runtime_manifest() -> dict[str, Any]:
    """Supply validated bundle inputs without making an artifact-verification claim."""
    return {
        "target": "phone",
        "profile": None,
        "build_type": "release",
        "transport": "usb-ncm",
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
    session = create_ready_session(runtime_root, status="prepared")
    Path(session["image"]).write_bytes(b"personalized RAM image")
    (root / "ramboot.bin").write_bytes(b"DHTBpayload")
    installed_rules = root / "installed-60-fplinux.rules"
    installed_rules.write_bytes(b"fixture-owned installed USB rules\n")
    shutil.copyfile(ROOT / "tests/fixtures/ram-tool/successful_bootstrap.sh", root / "ram-tool")
    (root / "ram-tool").chmod(0o755)
    tools = root / "bin"
    tools.mkdir()
    # Buffering is irrelevant for the local, silent RAM-tool process double.
    shutil.copyfile(ROOT / "tests/fixtures/runtime-tools/stdbuf.sh", tools / "stdbuf")
    (tools / "stdbuf").chmod(0o755)
    ssh_fixture = (
        ROOT / "tests/host_process/runtime/fixtures/loader-handoff/loader_ssh.py"
    ).read_text(encoding="utf-8")
    (tools / "ssh").write_text(f"#!{sys.executable}\n{ssh_fixture}", encoding="utf-8")
    (tools / "ssh").chmod(0o755)
    os.environ["PATH"] = f"{tools}{os.pathsep}{os.environ['PATH']}"
    os.environ["FPLINUX_LOADER_TEST_ROOT"] = str(root)
    os.environ["FPLINUX_LOADER_TEST_MODE"] = mode
    adapter = bundle_runtime.load_adapter(ROOT / "platforms/ums9117/host/adapter.py")
    manifest = runtime_manifest()

    def wait_for_session(
        prepared: dict[str, Any],
        *,
        expected_device_identity: str,
        on_linux_usb: Callable[[], None],
    ) -> dict[str, Any]:
        on_linux_usb()
        if mode == "authentication-failure":
            message = "controlled authentication failure"
            raise SystemExit(message)
        ready = {**prepared, "status": "ready"}
        ssh_transport.require_device_identity(ready, expected_device_identity)
        state = Path(ready["private_key"]).parent / "session.json"
        state.write_text(json.dumps(ready), encoding="utf-8")
        return ready

    with (
        mock.patch.object(runner, "load_adapter", return_value=adapter),
        mock.patch.object(runner, "USB_RULES_FILE", installed_rules),
        mock.patch.object(ssh_transport, "prepare_session", return_value=session),
        mock.patch.object(ssh_transport, "wait_for_bound_session", side_effect=wait_for_session),
        mock.patch.object(adapter, "usb_device_path", return_value=root / "absent-bootrom"),
        mock.patch.object(adapter, "require_usb_device_access"),
        loader_events.record_events(
            root / "events.jsonl", target="phone", profile=None, build_type="release"
        ) as events,
    ):
        admitted = bundle_runtime.ValidatedBundle(root, manifest, "8" * 64, "9" * 64, None)
        handoff = runner.run_bundle(admitted, events.emit, session_root=runtime_root)
    transport, ready = handoff
    if mode == "noninteractive":
        print("No interactive terminal is attached; the loader is complete.", flush=True)
    else:
        raise SystemExit(transport.run_shell(ready))


if __name__ == "__main__":
    main()

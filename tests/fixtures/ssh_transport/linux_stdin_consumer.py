# SPDX-License-Identifier: GPL-2.0-only
"""Forward process stdin through Linux identity probes and a real command transport."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from unittest import mock

from fplinux_cli.runtime import ssh_transport

from tests.fixtures.executables import install_python_script


def main() -> int:
    """Replace USB/network discovery and the SSH executable, retaining transport behavior."""
    state_path = Path(sys.argv[1])
    session = json.loads(state_path.read_text(encoding="utf-8"))
    probe = sys.argv[2]
    capture_output = sys.argv[3] == "capture"
    tools = state_path.parent / "tools"
    tools.mkdir()
    install_python_script(Path(__file__).with_name("stdin_peer.py"), tools / "ssh")

    with (
        mock.patch.object(ssh_transport, "_usb_devices", return_value=[Path("/usb/phone")]),
        mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb0"),
        mock.patch.object(ssh_transport, "_network_ready", return_value=True),
        mock.patch.dict(os.environ, {"PATH": f"{tools}:{os.environ.get('PATH', '')}"}),
    ):
        if probe in {"ram-session", "both"}:
            session = ssh_transport.reacquire_bound_session(
                session, expected_device_identity="12" * 32
            )
        if probe in {"kernel-identity", "both"}:
            ssh_transport.require_device_identity(session, "12" * 32)
        result = ssh_transport.run_remote(session, "receive-stdin", capture_output=capture_output)
    if capture_output:
        sys.stdout.write(result.stdout)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

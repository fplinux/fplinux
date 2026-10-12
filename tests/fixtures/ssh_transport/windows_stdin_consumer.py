# SPDX-License-Identifier: GPL-2.0-only
"""Forward process stdin through Windows transport methods with controlled host discovery."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from tests.fixtures.executables import install_python_script
from tests.windows_host_support import native_process_transport


def main() -> int:
    """Keep authentication and command processes real; native discovery and ACLs are stubs."""
    state_path = Path(sys.argv[1])
    session = json.loads(state_path.read_text(encoding="utf-8"))
    probe = sys.argv[2]
    capture_output = sys.argv[3] == "capture"
    transport = native_process_transport(state_path.parent)
    install_python_script(Path(__file__).with_name("stdin_peer.py"), transport.bundle.tools["ssh"])

    if probe in {"authentication", "both"}:
        session["status"] = "prepared"
        transport.authenticate(session, timeout=3, scan_host_key=False)
    if probe in {"metadata", "both"}:
        metadata = transport.run_remote(session, "uname -r", capture_output=True, shared=True)
        metadata.check_returncode()
    result = transport.run_remote(session, "receive-stdin", capture_output=capture_output)
    if capture_output:
        sys.stdout.write(result.stdout)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())

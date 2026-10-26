# SPDX-License-Identifier: GPL-2.0-only
"""Enter production interactive SSH with a local process as the external peer."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from fplinux_cli.runtime import ssh_transport
from fplinux_cli.runtime.connection import ConnectionLostError


def main() -> None:
    """Read the owned session and retain the consumer after its shell returns."""
    session = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    usb_state = Path(sys.argv[3])

    def usb_devices(session: dict[str, Any], *, require_absence_proof: bool = False) -> list[Path]:
        del session, require_absence_proof
        state = usb_state.read_text(encoding="ascii")
        if state == "unavailable":
            ssh_transport.fail("controlled USB discovery failure")
        if state == "ambiguous":
            return [Path("/usb/one"), Path("/usb/two")]
        return [Path("/usb/phone")] if state == "present" else []

    def local_peer(
        session: dict[str, Any],
        command: str | None = None,
        *,
        connect_timeout: int = 5,
    ) -> list[str]:
        del session, command, connect_timeout
        return [
            sys.executable,
            str(Path(__file__).with_name("interactive_peer.py")),
            sys.argv[2],
        ]

    ssh_transport._ssh_argv = local_peer  # noqa: SLF001 -- controlled SSH executable boundary.
    ssh_transport._usb_devices = usb_devices  # noqa: SLF001 -- controlled physical USB boundary.
    try:
        status = ssh_transport.run_shell(session)
    except ConnectionLostError:
        print("consumer observed USB disconnect", flush=True)
        status = 75
    except KeyboardInterrupt:
        status = 130
    except SystemExit as error:
        if not isinstance(error.code, int):
            raise
        status = error.code
    print(f"consumer returned: {status}", flush=True)
    raise SystemExit(status)


if __name__ == "__main__":
    main()

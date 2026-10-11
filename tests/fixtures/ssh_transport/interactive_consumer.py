# SPDX-License-Identifier: GPL-2.0-only
"""Enter production interactive SSH with a local process as the external peer."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from fplinux_cli.runtime import ssh_transport


def main() -> None:
    """Read the owned session and retain the consumer after its shell returns."""
    session = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))

    def local_peer(
        session: dict[str, Any],
        command: str | None = None,
        *,
        connect_timeout: int = 5,
        shared: bool = True,
    ) -> list[str]:
        del session, command, connect_timeout
        if shared:
            message = "interactive shell must own its connection"
            raise AssertionError(message)
        return [
            sys.executable,
            str(Path(__file__).with_name("interactive_peer.py")),
            *sys.argv[2:],
        ]

    ssh_transport._ssh_argv = local_peer  # noqa: SLF001 -- controlled SSH executable boundary.
    try:
        status = ssh_transport.run_shell(session)
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

# SPDX-License-Identifier: GPL-2.0-only
"""Supply only local terminal input, then execute the actual runtime entry point."""

from __future__ import annotations

import os
import pty
import sys
from pathlib import Path


def main() -> None:
    """Keep the terminal across checkout exec and select no behavior inside the runtime."""
    mode, source_path, checkout_path, bundle_path, action = sys.argv[1:]
    master, terminal = pty.openpty()
    try:
        os.dup2(terminal, 0)
        os.close(terminal)
        # The OS API makes the inheritable argument positional-only.
        os.set_inheritable(master, True)  # noqa: FBT003
        if mode == "checkout":
            sys.path.insert(0, str(Path(source_path) / "scripts"))
            # Only checkout selection imports the source tree before executing its bundle.
            from fplinux_cli import common  # noqa: PLC0415
            from fplinux_cli.runtime import runner  # noqa: PLC0415

            common.ROOT = Path(checkout_path)
            if action == "prepare":
                runner.prepare_target("nokia-ta1618", replace=True)
            else:
                runner.run_target("nokia-ta1618")
            return
        arguments = [sys.executable, "-I", str(Path(bundle_path) / "fplinux.py"), action]
        if action == "prepare":
            arguments.append("--replace")
        os.execv(sys.executable, arguments)
    finally:
        os.close(master)


if __name__ == "__main__":
    main()

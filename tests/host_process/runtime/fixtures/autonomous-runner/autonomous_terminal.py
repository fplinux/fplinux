# SPDX-License-Identifier: GPL-2.0-only
"""Give the delivered Linux launcher a local input terminal without a real SSH peer."""

from __future__ import annotations

import importlib.util
import os
import pty
import sys
from pathlib import Path
from unittest import mock


def main() -> None:
    """Keep terminal ownership local while executing the unmodified archive entry point."""
    master, terminal = pty.openpty()
    try:
        os.dup2(terminal, 0)
        os.close(terminal)
        launcher = Path(__file__).with_name("fplinux.py")
        sys.argv[0] = str(launcher)
        sys.argv.insert(1, "run")
        specification = importlib.util.spec_from_file_location("archive_launcher", launcher)
        assert specification is not None
        assert specification.loader is not None
        module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(module)
        runner = importlib.import_module("fplinux_cli.runtime.boot")
        with mock.patch.object(
            runner, "USB_RULES_FILE", launcher.parent / "boundary/usb-access.rules"
        ):
            module.main(bundle_root=launcher.parent)
    finally:
        os.close(master)


if __name__ == "__main__":
    main()

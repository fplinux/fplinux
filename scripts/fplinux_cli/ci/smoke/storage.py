# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001 -- Standalone native probe executed as a resource, not imported.
"""Run with the archive's Python to verify its native private storage behavior."""

from __future__ import annotations

import os
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(root / "scripts"))

from fplinux_cli.runtime.state import private_directory  # noqa: E402 -- delivered imports.

for relative in sys.argv[2:]:
    directory = private_directory(root / relative)
    probe = directory / "smoke-reopen.txt"
    with probe.open("a", encoding="ascii") as stream:
        stream.write("reopened\n")
    if os.name == "nt":
        from fplinux_cli.runtime.windows_host import WindowsHost

        host = WindowsHost()
        host.secure_new(probe)
        host.require_private(directory)
        host.require_private(probe)
    if "reopened\n" not in probe.read_text(encoding="ascii"):
        message = "native private file could not be reopened"
        raise SystemExit(message)

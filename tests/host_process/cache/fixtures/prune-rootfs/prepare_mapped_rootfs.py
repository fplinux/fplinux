# SPDX-License-Identifier: GPL-2.0-only
"""Create a package workspace with a mapped build-user owner."""

from __future__ import annotations

import os
import sys
from pathlib import Path

candidate = Path(sys.argv[1])
external = Path(sys.argv[2])
home = candidate / "package-work/home"
private = home / ".abuild"
private.mkdir(parents=True)
state = private / "build-state"
state.write_bytes(b"interrupted build\n")
(private / "external").symlink_to(external, target_is_directory=True)
for path in (state, private, home):
    os.chown(path, 1000, 1000)
home.chmod(0o755)
private.chmod(0o700)
state.chmod(0o600)

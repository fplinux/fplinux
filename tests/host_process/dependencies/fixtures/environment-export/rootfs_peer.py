# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and inspect numeric ownership inside the host's subordinate mapping."""

from __future__ import annotations

import json
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[2])
if sys.argv[1] == "prepare":
    (root / "usr/bin").mkdir(parents=True)
    (root / "home/builder/private").mkdir(parents=True)
    tool = root / "usr/bin/tool"
    tool.write_bytes(b"installed executable\n")
    os.chown(tool, 1234, 2345)
    tool.chmod(0o6755)
    private = root / "home/builder/private"
    os.chown(private, 1000, 1000)
    private.chmod(0o700)
    (private / "input").write_bytes(b"builder data\n")
    os.chown(private / "input", 1000, 1000)
    (root / "usr/bin/hard-link").hardlink_to(tool)
    (root / "tool").symlink_to("/usr/bin/tool")
elif sys.argv[1] == "inspect":
    tool = root / "usr/bin/tool"
    private = root / "home/builder/private"
    tool_stat = tool.stat()
    private_stat = private.stat()
    print(
        json.dumps(
            {
                "tool": [tool_stat.st_uid, tool_stat.st_gid, stat.S_IMODE(tool_stat.st_mode)],
                "private": [
                    private_stat.st_uid,
                    private_stat.st_gid,
                    stat.S_IMODE(private_stat.st_mode),
                ],
                "tool_bytes": tool.read_text(encoding="utf-8"),
                "private_bytes": (private / "input").read_text(encoding="utf-8"),
                "symlink": (root / "tool").readlink().as_posix(),
                "hard_link": tool_stat.st_ino == (root / "usr/bin/hard-link").stat().st_ino,
            }
        )
    )
else:
    message = "unknown rootfs fixture operation"
    raise SystemExit(message)

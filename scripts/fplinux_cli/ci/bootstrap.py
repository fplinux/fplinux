# SPDX-License-Identifier: GPL-2.0-only
"""Report the host prerequisites used by the rootless build runtime."""

from __future__ import annotations

import os
import platform
import pwd
import shutil
import subprocess
import sys
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.common import fail


def bootstrap_host() -> None:
    """Fail before restoration when the runner cannot support the existing runtime."""
    if sys.version_info[:2] != (3, 14):
        fail("CI requires Python 3.14")
    if platform.system() != "Linux" or platform.machine() not in {"x86_64", "AMD64"}:
        fail("CI builds require Linux x86-64")
    if os.geteuid() == 0:
        fail("CI builds must run as an ordinary user")
    for command in ("git", "gh", "tar", "zstd", "sha256sum", "newuidmap", "newgidmap", "unshare"):
        if shutil.which(command) is None:
            fail(f"CI host prerequisite is missing: {command}")
    account = pwd.getpwuid(os.getuid()).pw_name
    for name in ("subuid", "subgid"):
        path = Path("/etc") / name
        if not path.is_file():
            fail(f"CI requires a subordinate ID range in {path}")
        ranges = [line.split(":") for line in path.read_text().splitlines()]
        if not any(
            len(fields) == 3
            and fields[0] in {account, str(os.getuid())}
            and fields[2].isdigit()
            and int(fields[2]) >= 65536
            for fields in ranges
        ):
            fail(f"CI requires at least 65536 subordinate IDs for {account} in {path}")
    result = subprocess.run(
        ["unshare", "--user", "--map-root-user", "true"],
        capture_output=True,
        check=False,
        timeout=30,
    )
    if result.returncode:
        fail("CI host cannot create the user namespaces required by Kern")
    free = shutil.disk_usage(common.ROOT).free
    print(f"CI host prerequisites: ready; available disk: {free / 1024**3:.1f} GiB")

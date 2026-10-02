#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Install the isolated GLib development interface used by BlueZ."""

import json
import subprocess
import sys
from pathlib import Path


def main() -> None:
    """Copy private core headers and metadata from Meson's installed mapping."""
    package_dir = Path(sys.argv[1])
    core_paths = {
        "/usr/include/fplinux-bluez-glib-2.0/glib.h",
        "/usr/include/fplinux-bluez-glib-2.0/glib-unix.h",
        "/usr/lib/fplinux-bluez-glib-2.0/include/glibconfig.h",
        "/usr/lib/pkgconfig/fplinux-bluez-glib-2.0.pc",
    }
    for source_path, installed_path in json.load(sys.stdin).items():
        if (
            installed_path.startswith("/usr/include/fplinux-bluez-glib-2.0/glib/")
            or installed_path in core_paths
        ):
            destination = package_dir / installed_path.lstrip("/")
            subprocess.run(["install", "-Dm644", source_path, destination], check=True)


if __name__ == "__main__":
    main()

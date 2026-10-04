#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Install the static GLib development interface used by Bluetooth audio."""

import json
import re
import subprocess
import sys
from pathlib import Path


def main() -> None:
    """Copy the selected archives, headers and metadata from Meson's mapping."""
    package_dir = Path(sys.argv[1])
    atomic_archive = str(Path(sys.argv[2]) / "usr/lib/libatomic.a")
    libraries = {"glib-2.0", "gobject-2.0", "gmodule-2.0", "gio-2.0"}
    archives = {f"lib{name}.a" for name in libraries}
    metadata = libraries | {"gio-unix-2.0", "gmodule-no-export-2.0", "gmodule-export-2.0"}
    header_directories = (
        "/usr/include/glib-2.0/glib/",
        "/usr/include/glib-2.0/gobject/",
        "/usr/include/glib-2.0/gio/",
        "/usr/include/glib-2.0/gmodule/",
        "/usr/include/gio-unix-2.0/gio/",
    )
    headers = {
        "/usr/include/glib-2.0/glib.h",
        "/usr/include/glib-2.0/glib-unix.h",
        "/usr/include/glib-2.0/glib-object.h",
        "/usr/include/glib-2.0/gmodule.h",
        "/usr/lib/fplinux-glib-static/glib-2.0/include/glibconfig.h",
    }
    for source_path, installed_path in json.load(sys.stdin).items():
        installed = Path(installed_path)
        if (
            installed_path.startswith(header_directories)
            or installed_path in headers
            or installed.name in archives
        ):
            destination = package_dir / installed_path.lstrip("/")
        elif installed.suffix == ".pc" and installed.stem in metadata:
            destination = package_dir / "usr/lib/pkgconfig" / installed.name
        else:
            continue
        subprocess.run(["install", "-Dm644", source_path, destination], check=True)
        if installed.suffix == ".pc":
            # The consuming build owns its sysroot, so this library must be
            # resolved there instead of retaining the producer's archive path.
            content = re.sub(
                rf"(?<!\S){re.escape(atomic_archive)}(?!\S)",
                "-latomic",
                destination.read_text(),
            )
            destination.write_text(content)


if __name__ == "__main__":
    main()

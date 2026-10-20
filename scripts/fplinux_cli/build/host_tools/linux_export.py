# SPDX-License-Identifier: GPL-2.0-only
"""Export the standalone runtime from the read-only Linux build environment."""

from __future__ import annotations

import io
import json
import re
import subprocess
import sys
import tarfile
from pathlib import Path


def export_runtime() -> None:
    """Write regular runtime files and their installed package identities to stdout."""
    files = {"runtime/bin/python3.14": Path("/usr/bin/python3.14")}
    for name in ("ssh", "ssh-keygen", "ssh-keyscan"):
        files[f"runtime/bin/{name}"] = Path("/usr/bin") / name
    standard_library = Path("/usr/lib/python3.14")
    for path in sorted(standard_library.rglob("*")):
        standard_relative = path.relative_to(standard_library)
        if any(
            part in {"site-packages", "__pycache__", "config-3.14-x86_64-linux-musl"}
            for part in standard_relative.parts
        ):
            continue
        if path.is_file() and not path.name.startswith(
            (
                "_tkinter.",
                "readline.",
                "_gdbm.",
                "_dbm.",
                "_curses.",
                "_curses_panel.",
                "_sqlite3.",
            )
        ):
            files[f"runtime/lib/python3.14/{standard_relative.as_posix()}"] = path
    files["runtime/lib/ld-musl-x86_64.so.1"] = Path("/lib/ld-musl-x86_64.so.1")
    pending = list(files.values())
    inspected: set[Path] = set()
    while pending:
        path = pending.pop()
        resolved = path.resolve(strict=True)
        if resolved in inspected:
            continue
        inspected.add(resolved)
        with resolved.open("rb") as source:
            if source.read(4) != b"\x7fELF":
                continue
        result = subprocess.run(
            ["readelf", "-dW", str(resolved)], capture_output=True, text=True, check=True
        )
        for name in re.findall(r"\(NEEDED\).*\[([^\]]+)\]", result.stdout):
            relative = f"runtime/lib/{name}"
            if relative in files:
                continue
            candidates = [directory / name for directory in (Path("/lib"), Path("/usr/lib"))]
            library = next((candidate for candidate in candidates if candidate.is_file()), None)
            if library is None:
                raise RuntimeError(f"missing Linux runtime library: {name}")
            files[relative] = library
            pending.append(library)
    owned_paths = [
        str(path.resolve()) for path in files.values() if "python3.14/" not in str(path)
    ]
    owned_paths.append("/usr/bin/python3.14")
    owners = subprocess.run(
        ["apk", "info", "--no-network", "--who-owns", *sorted(set(owned_paths))],
        capture_output=True,
        text=True,
        check=True,
    )
    package_ids = {
        line.split(" is owned by ", 1)[1]
        for line in owners.stdout.splitlines()
        if " is owned by " in line
    }
    packages = []
    for record in Path("/lib/apk/db/installed").read_text().split("\n\n"):
        fields = dict(
            line.split(":", 1) for line in record.splitlines() if re.match(r"^[PVLUoc]:", line)
        )
        if f"{fields.get('P')}-{fields.get('V')}" in package_ids:
            packages.append(
                {
                    "name": fields["P"],
                    "version": fields["V"],
                    "license": fields.get("L", ""),
                    "url": fields.get("U", ""),
                    "origin": fields.get("o", fields["P"]),
                    "commit": fields.get("c", ""),
                }
            )
    with tarfile.open(fileobj=sys.stdout.buffer, mode="w|") as archive:
        for relative, path in sorted(files.items()):
            contents = path.read_bytes()
            member = tarfile.TarInfo(relative)
            member.size = len(contents)
            member.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
            archive.addfile(member, io.BytesIO(contents))
        contents = (
            json.dumps(sorted(packages, key=lambda package: package["name"]), sort_keys=True)
            + "\n"
        ).encode()
        member = tarfile.TarInfo("sources/linux-host-packages.json")
        member.size = len(contents)
        member.mode = 0o644
        archive.addfile(member, io.BytesIO(contents))


if __name__ == "__main__":
    export_runtime()

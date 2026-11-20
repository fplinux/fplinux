# SPDX-License-Identifier: GPL-2.0-only
"""Export the standalone runtime from the read-only Linux build environment."""

from __future__ import annotations

import io
import json
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path

OPENSSH_CLIENT_COMMANDS = ("ssh", "ssh-keygen", "ssh-keyscan", "sftp")


def _build_python(version: str, jobs: int) -> Path:
    work = Path("/work")
    with tarfile.open("/inputs/python.tar.xz") as source:
        source.extractall(work, filter="data")
    source_directory = work / f"Python-{version}"
    installation = work / "python"
    subprocess.run(
        [
            "./configure",
            "--quiet",
            "--prefix=/usr",
            "--enable-shared",
            "--with-system-expat",
            "--with-system-libmpdec",
            "--disable-test-modules",
            "--without-ensurepip",
        ],
        cwd=source_directory,
        # Use musl locale routines without linking the build environment's GNU gettext.
        env={
            **os.environ,
            "CFLAGS": "-O2 -g0",
            "ac_cv_header_libintl_h": "no",
            "ac_cv_lib_intl_textdomain": "no",
        },
        stdout=sys.stderr,
        check=True,
    )
    subprocess.run(
        ["make", "-s", f"-j{jobs}"], cwd=source_directory, stdout=sys.stderr, check=True
    )
    subprocess.run(
        ["make", "-s", f"DESTDIR={installation}", f"COMPILEALL_OPTS=-j{jobs} -q", "install"],
        cwd=source_directory,
        stdout=sys.stderr,
        check=True,
    )
    return installation / "usr"


def _python_files(installation: Path, abi: str) -> dict[str, Path]:
    interpreter = f"python{abi}"
    library = f"libpython{abi}.so.1.0"
    files = {
        f"runtime/bin/{interpreter}": installation / "bin" / interpreter,
        f"runtime/lib/{library}": installation / "lib" / library,
    }
    standard_library = installation / "lib" / interpreter
    for path in sorted(standard_library.rglob("*")):
        standard_relative = path.relative_to(standard_library)
        if any(
            part in {"site-packages", "__pycache__"} or part.startswith("config-")
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
                "_uuid.",
            )
        ):
            files[f"runtime/lib/{interpreter}/{standard_relative.as_posix()}"] = path
    return files


def _add_libraries(files: dict[str, Path]) -> None:
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


def _apk_packages(owned_paths: set[str]) -> list[dict[str, str]]:
    owners = subprocess.run(
        ["apk", "info", "--no-network", "--who-owns", *sorted(owned_paths)],
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
    return sorted(packages, key=lambda package: package["name"])


def export_runtime(version: str, jobs: int) -> None:
    """Build the archive interpreter and export it with the APK-provided runtime files."""
    installation = _build_python(version, jobs)
    files = _python_files(installation, version.rsplit(".", 1)[0])
    for name in OPENSSH_CLIENT_COMMANDS:
        files[f"runtime/bin/{name}"] = Path("/usr/bin") / name
    files["runtime/lib/ld-musl-x86_64.so.1"] = Path("/lib/ld-musl-x86_64.so.1")
    _add_libraries(files)
    packages = _apk_packages(
        {str(path.resolve()) for path in files.values() if not path.is_relative_to(installation)}
    )
    with tarfile.open(fileobj=sys.stdout.buffer, mode="w|") as archive:
        for relative, path in sorted(files.items()):
            contents = path.read_bytes()
            member = tarfile.TarInfo(relative)
            member.size = len(contents)
            member.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
            archive.addfile(member, io.BytesIO(contents))
        contents = (json.dumps(packages, sort_keys=True) + "\n").encode()
        member = tarfile.TarInfo("sources/linux-host-packages.json")
        member.size = len(contents)
        member.mode = 0o644
        archive.addfile(member, io.BytesIO(contents))


if __name__ == "__main__":
    export_runtime(sys.argv[1], int(sys.argv[2]))

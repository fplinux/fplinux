#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the MIDP natives and the VM glue of fplinux-phoneme into objects.

The source selection, include directories and defines come from the MIDP
native list (midp/natives.toml): upstream paths are relative to the patched
phoneME tree, project paths to the midp/ directory and generated paths to the
generated-source directory of build-midp-java.py. Extra sources (the VM glue),
source directories whose *.c files are all natives (port/<area>/), include
directories and compiler flags come from the command line. Every object is ARM code
(the interpreter enters natives with "mov pc"). The object paths are written,
one per line and in source order, to OUT/objects.txt.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import tomllib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

TARGET_FLAGS = (
    "--target=armv7-alpine-linux-musleabihf",
    "-mcpu=cortex-a7",
    "-mfpu=vfpv4",
    "-mfloat-abi=hard",
    "-marm",
    "-fno-pie",
    "-O2",
    "-fno-strict-aliasing",
    "-fno-delete-null-pointer-checks",
)


def string_list(table: dict[str, Any], key: str) -> list[str]:
    """Return one optional list of strings from a TOML table."""
    values = table.get(key, [])
    if not isinstance(values, list) or not all(isinstance(value, str) for value in values):
        message = f"natives.toml: {key} must be a list of strings"
        raise SystemExit(message)
    return list(values)


def compile_one(command: list[str]) -> tuple[list[str], subprocess.CompletedProcess[str]]:
    """Run one compiler command and keep its output for the log."""
    return command, subprocess.run(command, capture_output=True, text=True, check=False)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--natives", type=Path, required=True, help="midp/natives.toml")
    parser.add_argument("--src", type=Path, required=True, help="patched phoneME tree")
    parser.add_argument("--generated", type=Path, required=True, help="generated C sources")
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--include", type=Path, action="append", default=[])
    parser.add_argument("--extra-source", type=Path, action="append", default=[])
    parser.add_argument("--source-dir", type=Path, action="append", default=[])
    parser.add_argument("--cflag", action="append", default=[], help="extra compiler flag")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()

    with args.natives.open("rb") as stream:
        natives = tomllib.load(stream)
    project = args.natives.resolve().parent
    flags = natives.get("flags", {})
    includes = natives.get("include", {})
    sources = natives.get("sources", {})

    include_dirs = [
        *(args.src / path for path in string_list(includes, "upstream")),
        *(project / path for path in string_list(includes, "project")),
        args.generated,
        *args.include,
    ]
    # Upstream and generated code keeps its many old warnings quiet.
    source_files = [
        *((args.src / path, "-w") for path in string_list(sources, "upstream")),
        *((project / path, "-Wall") for path in string_list(sources, "project")),
        *((args.generated / path, "-w") for path in string_list(sources, "generated")),
        *((path, "-Wall") for path in args.extra_source),
        *(
            (path, "-Wall")
            for directory in args.source_dir
            for path in sorted(directory.glob("*.c"))
        ),
    ]
    common = [
        *TARGET_FLAGS,
        f"--sysroot={args.sysroot}",
        *string_list(flags, "defines"),
        *(f"-I{directory}" for directory in include_dirs),
        *args.cflag,
    ]
    c_flags = [f"-std={flags.get('c_std', 'gnu99')}", *string_list(flags, "c_extra")]
    cxx_flags = string_list(flags, "cxx_extra")

    args.out.mkdir(parents=True, exist_ok=True)
    commands: list[list[str]] = []
    objects: list[Path] = []
    for number, (source, warnings) in enumerate(source_files):
        obj = args.out / f"{number:03d}-{source.stem}.o"
        if source.suffix == ".cpp":
            compiler = ["clang++", *common, *cxx_flags]
        else:
            compiler = ["clang", *common, *c_flags]
        commands.append([*compiler, warnings, "-c", str(source), "-o", str(obj)])
        objects.append(obj)

    failures = 0
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as executor:
        for command, result in executor.map(compile_one, commands):
            if result.returncode != 0:
                failures += 1
                print("+ " + shlex.join(command), file=sys.stderr)
                print(result.stdout + result.stderr, file=sys.stderr)
    if failures:
        message = f"build-native: {failures} of {len(commands)} sources failed to compile"
        raise SystemExit(message)
    (args.out / "objects.txt").write_text("".join(f"{obj}\n" for obj in objects), encoding="utf-8")
    print(f"build-native: compiled {len(objects)} sources", flush=True)


if __name__ == "__main__":
    main()

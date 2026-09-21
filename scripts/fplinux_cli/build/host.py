# SPDX-License-Identifier: GPL-2.0-only
"""Build the host tools delivered with a target bundle."""

from __future__ import annotations

import os
import shlex
import shutil
import struct
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import Any

from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.inputs import fail
from fplinux_cli.build_env import build_environment
from fplinux_cli.common import sha256_file
from fplinux_cli.manifests.values import relative_value


def extract_host_members(
    archive: Path,
    prefix: str,
    members: list[dict[str, Any]],
    hashes: dict[str, Any],
    output: Path,
) -> None:
    """Extract and verify a typed host-tool source projection."""
    output.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as source:
        for item in members:
            relative = item["path"]
            expected = inputs_build.require_sha256(
                hashes.get(item["digest_key"]), f"host member {relative}"
            )
            member = source.getmember(prefix + relative)
            stream = source.extractfile(member)
            if stream is None or not member.isfile():
                fail(f"invalid host source member: {relative}")
            destination = output / Path(relative).name
            destination.write_bytes(stream.read())
            if sha256_file(destination) != expected:
                fail(f"host source hash mismatch: {relative}")


def copy_host_project_files(source: Path, copies: list[dict[str, str]]) -> None:
    """Copy declared project-owned host inputs beside verified upstream sources."""
    for step in copies:
        destination = source / relative_value(step["destination"], "host copy destination")
        if destination.exists() or destination.is_symlink():
            fail(f"host copy destination collides with verified source: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(
            inputs_build.require_file(inputs_build.root_source(step["source"])), destination
        )


def build_make_host_tool(
    sources: dict[str, Any],
    recipe: dict[str, Any],
    work: Path,
    output: Path,
) -> Path:
    """Build one pinned make-archive host recipe."""
    source_lock = sources_build.source_lock_entry(sources, recipe["source_lock"])
    archive = sources_build.fetch(
        source_lock.get("archive_url"),
        source_lock.get("archive_sha256"),
        inputs_build.CACHE / "downloads",
        recipe["cache_name"],
    )
    commit = source_lock.get("commit")
    hashes = source_lock.get("files")
    if not isinstance(commit, str) or not commit:
        fail(f"host source {recipe['source_lock']} commit must be a non-empty string")
    if not isinstance(hashes, dict):
        fail(f"host source {recipe['source_lock']} files table is missing")
    prefix = recipe["archive_prefix"].replace("{commit}", commit)
    with tempfile.TemporaryDirectory(
        dir=work,
        prefix=f".{recipe['source_directory']}.",
    ) as temporary:
        source = Path(temporary) / recipe["source_directory"]
        extract_host_members(archive, prefix, recipe["members"], hashes, source)
        copy_host_project_files(source, recipe["copies"])
        sources_build.apply_patches(
            source, [inputs_build.root_source(path) for path in recipe["patches"]]
        )
        make_arguments = ["LIBS=-static -lusb-1.0"] if recipe["link"] == "static-libusb" else []
        process_build.run(["make", "-C", str(source), "clean", "all", *make_arguments])
        built = inputs_build.require_file(source / recipe["binary"])
        if recipe["self_test"]:
            process_build.run([str(built), "--self-test"])
        destination: Path = output / recipe["name"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(built, destination)
        destination.chmod(0o755)
        return destination


def _verify_static_host_binary(path: Path) -> None:
    """Require a portable static Linux/x86-64 ELF with no shared dependencies."""
    data = inputs_build.require_file(path).read_bytes()
    if (
        len(data) < 64
        or data[:4] != b"\x7fELF"
        or data[4] != 2
        or data[5] != 1
        or struct.unpack_from("<H", data, 18)[0] != 62
    ):
        fail(f"host tool is not a little-endian Linux/x86-64 ELF: {path}")
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    program_size = struct.unpack_from("<H", data, 54)[0]
    program_count = struct.unpack_from("<H", data, 56)[0]
    if program_size < 4 or program_offset + program_size * program_count > len(data):
        fail(f"host tool has an invalid ELF program-header table: {path}")
    for index in range(program_count):
        offset = program_offset + index * program_size
        if struct.unpack_from("<I", data, offset)[0] == 3:
            fail(f"host tool must not require a dynamic ELF interpreter: {path}")

    dynamic = subprocess.run(
        ["readelf", "-dW", str(path)],
        capture_output=True,
        text=True,
        env=build_environment(),
        check=False,
    )
    if dynamic.returncode != 0:
        fail(dynamic.stderr.strip() or f"cannot inspect host ELF dependencies: {path}")
    if "(NEEDED)" in dynamic.stdout:
        fail(f"host tool must not have DT_NEEDED dependencies: {path}")


def build_cc_libusb_tool(recipe: dict[str, Any], output: Path) -> Path:
    """Build one portable static C/libusb host-tool recipe."""
    pkg = subprocess.run(
        ["pkg-config", "--cflags", "--static", "--libs", "libusb-1.0"],
        capture_output=True,
        text=True,
        env=build_environment(),
        check=False,
    )
    if pkg.returncode:
        fail(pkg.stderr.strip())
    destination: Path = output / recipe["name"]
    process_build.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-O2",
            "-g0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fno-ident",
            "-static",
            "-Wl,--gc-sections",
            "-I",
            str(common.ROOT / "include/fplinux"),
            "-o",
            str(destination),
            str(inputs_build.require_file(inputs_build.root_source(recipe["source"]))),
            str(inputs_build.require_file(inputs_build.root_source("lib/fplinux/fplinux-cli.c"))),
            *shlex.split(pkg.stdout),
            "-pthread",
        ]
    )
    destination.chmod(0o755)
    if recipe["self_test"]:
        process_build.run([str(destination), "--self-test"])
    return destination


def build_host_tools(
    sources: dict[str, Any], platform: dict[str, Any], work: Path
) -> dict[str, Path]:
    """Build every typed platform host-tool recipe."""
    source_work = work / "host-build"
    output = work / "host"
    source_work.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    result: dict[str, Path] = {}
    for recipe in platform["host"]["tools"]:
        if recipe["type"] == "make-archive":
            built = build_make_host_tool(sources, recipe, source_work, output)
        elif recipe["type"] == "cc-libusb":
            built = build_cc_libusb_tool(recipe, output)
        else:
            fail(f"unsupported host recipe type: {recipe['type']}")
        _verify_static_host_binary(built)
        result[recipe["name"]] = built
    return result

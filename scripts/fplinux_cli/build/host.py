# SPDX-License-Identifier: GPL-2.0-only
"""Build the host tools delivered with a target bundle."""

from __future__ import annotations

import os
import shlex
import shutil
import stat
import struct
import subprocess
import tarfile
import tempfile
from pathlib import Path
from typing import Any

from fplinux_cli import build_env, common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build
from fplinux_cli.common import (
    canonical_json_bytes,
    fail,
    read_json_object,
    replace_file_atomically,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.manifests.values import relative_value

_COMPILER_ENVIRONMENT = (
    "AR",
    "AS",
    "CC",
    "CFLAGS",
    "C_INCLUDE_PATH",
    "CPATH",
    "CPPFLAGS",
    "LD",
    "LDFLAGS",
    "LD_LIBRARY_PATH",
    "LIBRARY_PATH",
    "LIBS",
    "MAKEFLAGS",
    "PATH",
    "PKG_CONFIG",
    "PKG_CONFIG_LIBDIR",
    "PKG_CONFIG_PATH",
    "PKG_CONFIG_SYSROOT_DIR",
    "RANLIB",
)


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
        env=build_env.build_environment(),
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
        env=build_env.build_environment(),
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


def _host_tool_recipe(sources: dict[str, Any], recipe: dict[str, Any]) -> str:
    """Identify only the source projection and toolchain consumed by one tool."""
    local_files: dict[str, str] = {}
    if recipe["type"] == "make-archive":
        source_lock = sources_build.source_lock_entry(sources, recipe["source_lock"])
        commit = source_lock.get("commit")
        hashes = source_lock.get("files")
        if not isinstance(commit, str) or not commit:
            fail(f"host source {recipe['source_lock']} commit must be a non-empty string")
        if not isinstance(hashes, dict):
            fail(f"host source {recipe['source_lock']} files table is missing")
        archive_url = source_lock.get("archive_url")
        if not isinstance(archive_url, str) or not archive_url.startswith("https://"):
            fail("source URL must be a non-empty HTTPS URL")
        inputs_build.require_sha256(source_lock.get("archive_sha256"), "host source archive")
        members = {
            item["path"]: inputs_build.require_sha256(
                hashes.get(item["digest_key"]), f"host member {item['path']}"
            )
            for item in recipe["members"]
        }
        for step in recipe["copies"]:
            relative = step["source"]
            local_files[relative] = sha256_file(
                inputs_build.require_file(inputs_build.root_source(relative))
            )
        for relative in recipe["patches"]:
            local_files[relative] = sha256_file(
                inputs_build.require_file(inputs_build.root_source(relative))
            )
        upstream: dict[str, Any] = {"commit": commit, "members": members}
    elif recipe["type"] == "cc-libusb":
        for relative in (
            recipe["source"],
            "lib/fplinux/fplinux-cli.c",
            "include/fplinux/fplinux-cli.h",
        ):
            local_files[relative] = sha256_file(
                inputs_build.require_file(inputs_build.root_source(relative))
            )
        upstream = {}
    else:
        fail(f"unsupported host recipe type: {recipe['type']}")

    implementation = {
        path.name: sha256_file(path)
        for path in (
            Path(__file__),
            Path(inputs_build.__file__),
            Path(process_build.__file__),
            Path(sources_build.__file__),
            Path(build_env.__file__),
        )
    }
    image_recipe, image_content = inputs_build.container_image_environment()
    payload = {
        "recipe": recipe,
        "upstream": upstream,
        "local_files": local_files,
        "implementation": implementation,
        "compiler_environment": {
            name: os.environ[name] for name in _COMPILER_ENVIRONMENT if name in os.environ
        },
        "image_recipe": image_recipe,
        "image_content": image_content,
    }
    return sha256_bytes(canonical_json_bytes(payload))


def _cached_host_tool(
    slot: Path, recipe_digest: str, recipe: dict[str, Any], output: Path
) -> Path | None:
    """Copy a complete cached binary into this build after current validation."""
    receipt = slot / "receipt.json"
    binary = slot / "binary"
    if receipt.is_symlink() or binary.is_symlink() or not binary.is_file():
        return None
    record = read_json_object(receipt)
    if record is None or record.get("recipe") != recipe_digest:
        return None
    if stat.S_IMODE(binary.stat().st_mode) != 0o755 or sha256_file(binary) != record.get("sha256"):
        return None
    try:
        _verify_static_host_binary(binary)
        destination: Path = output / recipe["name"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(binary, destination)
        destination.chmod(0o755)
        if recipe["self_test"]:
            process_build.run([str(destination), "--self-test"])
    except OSError, subprocess.CalledProcessError, SystemExit:
        return None
    return destination


def _publish_host_tool(slot: Path, recipe_digest: str, binary: Path) -> None:
    """Publish the verified binary before its receipt; a split pair is a miss."""
    slot.mkdir(parents=True, exist_ok=True)
    contents = binary.read_bytes()
    replace_file_atomically(slot / "binary", contents, 0o755, sync=False)
    receipt = canonical_json_bytes({"recipe": recipe_digest, "sha256": sha256_bytes(contents)})
    replace_file_atomically(slot / "receipt.json", receipt, 0o644, sync=False)


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
        recipe_digest = _host_tool_recipe(sources, recipe)
        cache_name = relative_value(recipe["name"], "host tool name")
        slot = inputs_build.CACHE / "host-tools" / cache_name
        cached = _cached_host_tool(slot, recipe_digest, recipe, output)
        if cached is not None:
            process_build.log_message(f"Host tool {recipe['name']}: cached")
            result[recipe["name"]] = cached
            continue
        process_build.log_message(f"Host tool {recipe['name']}: building")
        if recipe["type"] == "make-archive":
            built = build_make_host_tool(sources, recipe, source_work, output)
        elif recipe["type"] == "cc-libusb":
            built = build_cc_libusb_tool(recipe, output)
        else:
            fail(f"unsupported host recipe type: {recipe['type']}")
        _verify_static_host_binary(built)
        _publish_host_tool(slot, recipe_digest, built)
        result[recipe["name"]] = built
    return result

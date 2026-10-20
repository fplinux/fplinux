# SPDX-License-Identifier: GPL-2.0-only
"""Collect project sources and exact upstream inputs for a separate source archive."""

from __future__ import annotations

import io
import re
import subprocess
import tarfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail
from fplinux_cli.environment.downloads import download_locked_file

if TYPE_CHECKING:
    from collections.abc import Sequence

    from fplinux_cli.dependencies.inputs import DependencyInput
    from fplinux_cli.manifests.loaders import LoaderSource

_PROJECT_DIRECTORIES = frozenset(
    {
        "LICENSES",
        "third-party-licenses",
        "alpine",
        "bootstrap",
        "common",
        "include",
        "lib",
        "platforms",
        "profiles",
        "scripts",
        "site",
        "targets",
    }
)
_PROJECT_ROOT_FILES = frozenset(
    {
        ".clang-format",
        ".editorconfig",
        ".gitattributes",
        ".gitignore",
        ".kernignore",
        ".taplo.toml",
        "Containerfile",
        "LICENSE",
        "README.md",
        "REUSE.toml",
        "THIRD_PARTY_NOTICES.md",
        "fplinux",
        "package-lock.json",
        "package.json",
        "pyproject.toml",
    }
)
_PRIVATE_COMPONENTS = frozenset(
    {
        "__pycache__",
        "diagnostics",
        "dist",
        "dumps",
        "evidence",
        "handoffs",
        "keys",
        "node_modules",
        "private",
        "research",
        "signing",
    }
)
_NON_SOURCE_SUFFIXES = frozenset(
    {
        ".apk",
        ".bin",
        ".cpio",
        ".dll",
        ".dump",
        ".elf",
        ".exe",
        ".key",
        ".pem",
        ".pub",
        ".pyc",
        ".pyo",
    }
)


@dataclass(frozen=True)
class SourcePayload:
    """Source archive members, executable modes and notices for the runnable archive."""

    files: dict[str, bytes]
    executables: set[str]
    notices: dict[str, bytes]


def _git_paths(*arguments: str) -> set[str]:
    result = subprocess.run(
        ["git", "-C", str(common.ROOT), *arguments, "-z"],
        capture_output=True,
        check=False,
    )
    if result.returncode:
        fail("source packaging requires the active Git checkout")
    return {name.decode("utf-8") for name in result.stdout.split(b"\0") if name}


def _project_source(relative: str) -> bool:
    path = Path(relative)
    if "AGENTS.md" in path.parts or any(part in _PRIVATE_COMPONENTS for part in path.parts):
        return False
    if path.name.startswith(".env") or path.suffix.lower() in _NON_SOURCE_SUFFIXES:
        return False
    if len(path.parts) == 1:
        return relative in _PROJECT_ROOT_FILES or (
            not relative.startswith(".") and relative.endswith(".lock.toml")
        )
    if any(part.startswith(".") for part in path.parts):
        return False
    return path.parts[0] in _PROJECT_DIRECTORIES


def project_source_files() -> SourcePayload:
    """Read active tracked and nonignored project source bytes, retaining executable modes."""
    tracked = _git_paths("ls-files", "--cached")
    new = _git_paths("ls-files", "--others", "--exclude-standard")
    deleted = _git_paths("diff", "--cached", "--name-only", "--diff-filter=D")
    files = {}
    executables = set()
    for relative in sorted((tracked | new) - deleted):
        if not _project_source(relative):
            continue
        source = common.ROOT / relative
        if not source.exists() and not source.is_symlink():
            continue
        current = common.ROOT
        for part in Path(relative).parts:
            current /= part
            if current.is_symlink():
                fail(f"source archive input must not traverse a symlink: {relative}")
        if not source.is_file():
            fail(f"source archive input must be a regular file: {relative}")
        member = f"project/{relative}"
        files[member] = source.read_bytes()
        if source.stat().st_mode & 0o111:
            executables.add(member)
    return SourcePayload(files, executables, {})


def _archive_input(
    *, name: str, entry: dict[str, Any], destination: str, purpose: str
) -> DependencyInput:
    from fplinux_cli.dependencies.inputs import DependencyInput  # noqa: PLC0415

    return DependencyInput(
        key=name,
        url=entry["archive_url"] if "archive_url" in entry else entry["url"],
        sha256=entry.get("archive_sha256", entry.get("sha256")),
        size=None,
        destination=destination,
        purpose=purpose,
    )


def _build_sources(config: dict[str, Any], platform: dict[str, Any]) -> list[DependencyInput]:
    from fplinux_cli.build.sources import source_lock_entry  # noqa: PLC0415
    from fplinux_cli.dependencies.inputs import DependencyInput  # noqa: PLC0415
    from fplinux_cli.device_data.prepare import NAND_ECC_TOOL  # noqa: PLC0415
    from fplinux_cli.manifests.loaders import loader_sources  # noqa: PLC0415

    sources = common.load_toml(common.ROOT / "sources.lock.toml")
    linux = source_lock_entry(sources, platform["linux"]["source_lock"])
    result = [
        _archive_input(
            name="linux",
            entry=linux,
            destination=f"downloads/linux/linux-{linux['version']}.tar.xz",
            purpose="linux-source",
        ),
    ]
    if platform["bootstrap"]["backend"] == "sc9820e":
        result.extend(
            DependencyInput(
                key=f"loader:{source.role}",
                url=source.url,
                sha256=source.sha256,
                size=None,
                destination=f"downloads/{source.cache_name}",
                purpose="loader-source",
            )
            for source in loader_sources(config["loader"]["build"], sources)
        )
    else:
        bootstrap = platform["bootstrap"]
        result.append(
            _archive_input(
                name="bootstrap",
                entry=source_lock_entry(sources, bootstrap["vendor_source_lock"]),
                destination=f"downloads/{bootstrap['vendor_cache_name']}",
                purpose="bootstrap-source",
            )
        )
    runtime_tools = platform["host"]["runtime_tools"]
    selected = {runtime_tools[role] for role in ("loader", "bridge") if role in runtime_tools}
    if (config.get("nand") or {}).get("ecc_format"):
        selected.add(NAND_ECC_TOOL)
    archive_tools = [
        recipe
        for recipe in platform["host"]["tools"]
        if recipe["type"] == "make-archive" and recipe["name"] in selected
    ]
    result.extend(
        _archive_input(
            name=f"host:{recipe['name']}",
            entry=source_lock_entry(sources, recipe["source_lock"]),
            destination=f"downloads/{recipe['cache_name']}",
            purpose="host-source",
        )
        for recipe in archive_tools
    )
    return result


def _collect_archives(
    inputs: Sequence[DependencyInput], files: dict[str, bytes], *, offline: bool
) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    for item in inputs:
        digest = item.checksum if item.checksum is not None else item.sha256
        if digest is None:
            fail(f"source input has no checksum: {item.key}")
        path = download_locked_file(
            item.url,
            digest,
            common.ROOT / ".cache" / item.destination,
            offline=offline,
            algorithm=item.algorithm,
            size=item.size,
        )
        member = f"upstream/{item.destination.removeprefix('downloads/')}"
        if member not in files:
            files[member] = path.read_bytes()
        records.append(
            {
                "component": item.key,
                "archive": member,
                "url": item.url,
                "algorithm": item.algorithm,
                "checksum": digest,
            }
        )
    return records


def _loader_license_notices(
    sources: Sequence[LoaderSource], files: dict[str, bytes]
) -> dict[str, bytes]:
    """Retain the board initializer and secure monitor's original license files."""
    notices = {}
    for source in sources:
        if source.role == "fdl1":
            continue
        member = f"upstream/{source.cache_name}"
        with tarfile.open(fileobj=io.BytesIO(files[member])) as archive:
            if source.role == "fdl2":
                prefix = f"{source.archive_prefix}/Licenses/"
                selected = [
                    item
                    for item in archive.getmembers()
                    if item.isfile() and item.name.startswith(prefix)
                ]
                if not selected:
                    fail("RAM board initializer license files are missing")
            else:
                prefix = f"{source.archive_prefix}/"
                selected = [
                    item
                    for item in archive.getmembers()
                    if item.isfile() and item.name == f"{prefix}license.md"
                ]
                if not selected:
                    fail("RAM secure monitor license is missing")
            for item in selected:
                stream = archive.extractfile(item)
                if stream is None:
                    fail(f"RAM loader license is not a regular file: {item.name}")
                name = item.name.removeprefix(prefix)
                notices[f"licenses/loaders/{source.role}/{name}"] = stream.read()
    return notices


def collect_loader_license_notices(config: dict[str, Any]) -> dict[str, bytes]:
    """Read ordinary binary notices from the admitted cached loader archives."""
    from fplinux_cli.manifests.loaders import loader_sources  # noqa: PLC0415

    sources = loader_sources(
        config["loader"]["build"], common.load_toml(common.ROOT / "sources.lock.toml")
    )
    archives = {}
    for source in sources:
        if source.role == "fdl1":
            continue
        path = download_locked_file(
            source.url,
            source.sha256,
            common.ROOT / ".cache/downloads" / source.cache_name,
            offline=True,
        )
        archives[f"upstream/{source.cache_name}"] = path.read_bytes()
    return _loader_license_notices(sources, archives)


def _linux_runtime_sources(
    files: dict[str, bytes], notices: dict[str, bytes], *, offline: bool
) -> list[dict[str, object]]:
    lock = common.load_toml(common.ROOT / "linux-host.lock.toml")
    inputs = [
        _archive_input(
            name=f"linux-host:{name}",
            entry=entry,
            destination=f"linux-host/downloads/{entry['name']}",
            purpose="host-runtime-source",
        )
        for name, entry in sorted(lock.items())
    ]
    records = _collect_archives(inputs, files, offline=offline)
    libusb = lock["libusb"]
    member = f"upstream/linux-host/downloads/{libusb['name']}"
    with tarfile.open(fileobj=io.BytesIO(files[member])) as archive:
        stream = archive.extractfile(libusb["licenses"][0])
        if stream is None:
            fail("Linux host libusb license is missing")
        notices["licenses/host-tools/libusb-COPYING.txt"] = stream.read()
    return records


def _package_source_channel(info: str, *, repository: str) -> dict[str, str]:
    fields = {}
    for line in info.splitlines():
        name, separator, value = line.partition(" = ")
        if separator:
            fields[name] = value
    origin = fields.get("origin", fields.get("pkgname", ""))
    commit = fields.get("commit", "")
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None or not origin:
        fail("Alpine package does not declare an exact source revision")
    return {
        "name": fields["pkgname"],
        "version": fields["pkgver"],
        "license": fields.get("license", ""),
        "origin": origin,
        "source": f"https://gitlab.alpinelinux.org/alpine/aports/-/tree/{commit}/{repository}/{origin}",
    }


def _alpine_source_channels(
    packages: Sequence[str], files: dict[str, bytes], *, offline: bool
) -> None:
    from fplinux_cli.alpine.lock import load_alpine_lock, runtime_package_names  # noqa: PLC0415

    lock = load_alpine_lock(common.ROOT)
    selected = set(runtime_package_names(lock, packages))
    records = []
    base = download_locked_file(
        lock["minirootfs"]["url"],
        lock["minirootfs"]["sha256"],
        common.ROOT
        / ".cache/downloads/alpine"
        / f"alpine-minirootfs-{lock['release']}-{lock['arch']}.tar.gz",
        offline=offline,
        size=lock["minirootfs"]["bytes"],
    )
    with tarfile.open(base) as archive:
        stream = archive.extractfile("./lib/apk/db/installed")
        if stream is None:
            fail("Alpine base package source metadata is missing")
        for block in stream.read().decode().split("\n\n"):
            fields = dict(
                line.split(":", 1) for line in block.splitlines() if re.match(r"^[PVLoc]:", line)
            )
            if not fields:
                continue
            info = "\n".join(
                f"{key} = {fields.get(field, '')}"
                for key, field in (
                    ("pkgname", "P"),
                    ("pkgver", "V"),
                    ("license", "L"),
                    ("origin", "o"),
                    ("commit", "c"),
                )
            )
            records.append(_package_source_channel(info, repository="main"))
    for entry in lock["package"]:
        if entry["file"] not in selected:
            continue
        repository = entry["repository"]
        path = download_locked_file(
            f"{lock['repositories'][repository]}/{entry['file']}",
            entry["sha256"],
            common.ROOT / ".cache/downloads/alpine/packages" / entry["file"],
            offline=offline,
            size=entry["bytes"],
        )
        with tarfile.open(path) as archive:
            stream = archive.extractfile(".PKGINFO")
            if stream is None:
                fail(f"Alpine source metadata is missing: {entry['file']}")
            records.append(_package_source_channel(stream.read().decode(), repository=repository))
    files["upstream/alpine-source-channels.json"] = canonical_json_bytes(records)


def collect_source_files(config: dict[str, Any], *, host: str, offline: bool) -> SourcePayload:
    """Collect matching source material while the caller owns the project cache lock."""
    from fplinux_cli.alpine.selection import (  # noqa: PLC0415
        aport_build_order,
        selected_packages,
    )
    from fplinux_cli.dependencies.inputs import _apkbuild_declarations  # noqa: PLC0415
    from fplinux_cli.manifests.platforms import load_platform  # noqa: PLC0415

    payload = project_source_files()
    platform = load_platform(config["platform"])
    rootfs = selected_packages(platform, config, common.ROOT)
    packages = tuple(rootfs)
    inputs = _build_sources(config, platform)
    for producer in aport_build_order(packages):
        _, source_inputs = _apkbuild_declarations(
            common.ROOT / "alpine/aports" / producer / "APKBUILD"
        )
        inputs.extend(source_inputs)
    records = _collect_archives(inputs, payload.files, offline=offline)
    if platform["bootstrap"]["backend"] == "sc9820e":
        from fplinux_cli.manifests.loaders import loader_sources  # noqa: PLC0415

        sources = common.load_toml(common.ROOT / "sources.lock.toml")
        payload.notices.update(
            _loader_license_notices(
                loader_sources(config["loader"]["build"], sources), payload.files
            )
        )
    if host == "linux":
        records.extend(_linux_runtime_sources(payload.files, payload.notices, offline=offline))
    else:
        _windows_source_channels(payload.files)
    _alpine_source_channels(packages, payload.files, offline=offline)
    payload.files["upstream/downloads.json"] = canonical_json_bytes(records)
    if host == "linux":
        payload.files["BUILD-LINUX.txt"] = _linux_build_instructions()
    payload.files.update(payload.notices)
    return payload


def _linux_build_instructions() -> bytes:
    return (Path(__file__).with_name("sources") / "BUILD-LINUX.txt").read_bytes()


def _windows_source_channels(files: dict[str, bytes]) -> None:
    lock = common.load_toml(common.ROOT / "windows-host.lock.toml")
    python = lock["python"]["version"]
    openssh = lock["openssh"]["version"]
    source_version = openssh.partition("p")[0]
    source_root = f"https://github.com/PowerShell/openssh-portable/tree/v{source_version}"
    files["upstream/windows-runtime-source-channels.json"] = canonical_json_bytes(
        {
            "python": {
                "version": python,
                "source": f"https://www.python.org/ftp/python/{python}/Python-{python}.tar.xz",
            },
            "openssh": {
                "version": openssh,
                "source": source_root,
                "dependencies": f"{source_root}/contrib/win32/openssh/vcpkg.json",
            },
        }
    )

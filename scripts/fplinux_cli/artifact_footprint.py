# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- artifact diagnostics are local to this reader.
"""Describe measured bundle layers and package-owned root filesystem content."""

from __future__ import annotations

import gzip
import json
import lzma
import re
import stat
import struct
import tarfile
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import TYPE_CHECKING, Any

from .common import canonical_json_bytes, sha256_bytes, sha256_file
from .manifests.kernel import kconfig_values

if TYPE_CHECKING:
    from pathlib import Path

    from .bundle_state import CurrentBundle


class FootprintError(RuntimeError):
    """The selected bundle cannot provide a verified footprint."""


@dataclass(frozen=True)
class _Entry:
    name: str
    mode: int
    uid: int
    gid: int
    inode: tuple[int, int, int]
    links: int
    device: tuple[int, int]
    data: bytes


def _cpio_entries(data: bytes) -> list[_Entry]:
    entries: list[_Entry] = []
    offset = 0
    while offset + 110 <= len(data):
        header = data[offset : offset + 110]
        if header[:6] != b"070701":
            raise FootprintError("root filesystem is not a newc archive")
        fields = [int(header[index : index + 8], 16) for index in range(6, 110, 8)]
        inode, mode, uid, gid, links, _mtime, size, major, minor, rmajor, rminor, namesize, _ = (
            fields
        )
        name_end = offset + 110 + namesize
        payload_start = (name_end + 3) & ~3
        payload_end = payload_start + size
        if not namesize or payload_end > len(data) or data[name_end - 1] != 0:
            raise FootprintError("root filesystem archive is truncated")
        name = data[offset + 110 : name_end - 1].decode("utf-8")
        if name == "TRAILER!!!":
            return entries
        name = name.removeprefix("./")
        entries.append(
            _Entry(
                name,
                mode,
                uid,
                gid,
                (major, minor, inode),
                links,
                (rmajor, rminor),
                data[payload_start:payload_end],
            )
        )
        offset = (payload_end + 3) & ~3
    raise FootprintError("root filesystem archive has no trailer")


def _installed_packages(data: bytes) -> tuple[dict[str, dict[str, Any]], dict[str, str]]:
    packages: dict[str, dict[str, Any]] = {}
    owners: dict[str, str] = {}
    for paragraph in data.decode("utf-8").split("\n\n"):
        if not paragraph.strip():
            continue
        fields: dict[str, str] = {}
        paths = []
        directory = ""
        for line in paragraph.splitlines():
            key, separator, value = line.partition(":")
            if not separator:
                raise FootprintError("invalid installed package database")
            if key == "F":
                directory = value
            elif key == "R":
                paths.append(str(PurePosixPath(directory, value)))
            elif key in {"P", "V", "D", "p", "i"}:
                fields[key] = value
        name = fields["P"]
        packages[name] = {
            "version": fields["V"],
            "dependencies": fields.get("D", "").split(),
            "provides": fields.get("p", "").split(),
            "install_if": fields.get("i", "").split(),
            "regular_payload_bytes": 0,
            "symlink_payload_bytes": 0,
            "files": [],
        }
        for path in paths:
            owners[path] = name
    return packages, owners


def _dependency_name(requirement: str) -> str:
    return re.split(r"[<>=~@]", requirement, maxsplit=1)[0]


def _providers(
    packages: dict[str, dict[str, Any]],
    owners: dict[str, str],
) -> dict[str, list[str]]:
    providers: dict[str, list[str]] = defaultdict(list)
    for name, package in packages.items():
        for capability in [name, *package["provides"]]:
            key = _dependency_name(capability)
            if name not in providers[key]:
                providers[key].append(name)
    for path, owner in owners.items():
        providers["/" + path] = [owner]
    return providers


def _dependency_edges(
    requirements: list[str],
    providers: dict[str, list[str]],
) -> list[dict[str, Any]]:
    # The installed database is the solver's result. Do not solve a new package
    # set or pretend to disambiguate multiple installed virtual providers.
    return [
        {"requirement": requirement, "providers": providers.get(_dependency_name(requirement), [])}
        for requirement in requirements
        if not requirement.startswith("!")
    ]


def _explain_dependencies(
    packages: dict[str, dict[str, Any]],
    world: list[str],
    providers: dict[str, list[str]],
) -> None:
    automatic: dict[str, list[str]] = defaultdict(list)
    for name, package in packages.items():
        package["dependency_edges"] = _dependency_edges(package["dependencies"], providers)
        conditions = _dependency_edges(package["install_if"], providers)
        package["install_if_edges"] = conditions
        if conditions and all(edge["providers"] for edge in conditions):
            for edge in conditions:
                for provider in edge["providers"]:
                    automatic[provider].append(name)
        package["selected_by"] = []
        package["reason_paths"] = {}
    for selection in world:
        roots = providers.get(_dependency_name(selection), [])
        queue = deque((name, [name]) for name in roots)
        seen: set[str] = set()
        for name in roots:
            packages[name]["selected_by"].append(selection)
        while queue:
            name, path = queue.popleft()
            if name in seen:
                continue
            seen.add(name)
            packages[name]["reason_paths"][selection] = path
            for edge in packages[name]["dependency_edges"]:
                queue.extend((provider, [*path, provider]) for provider in edge["providers"])
            queue.extend((installed, [*path, installed]) for installed in automatic[name])


def _rootfs_report(data: bytes, *, external: bool) -> tuple[dict[str, Any], dict[str, list[str]]]:
    entries = _cpio_entries(data)
    contents = {entry.name: entry.data for entry in entries}
    packages, owners = _installed_packages(contents["lib/apk/db/installed"])
    world = contents["etc/apk/world"].decode("utf-8").splitlines()
    providers = _providers(packages, owners)
    _explain_dependencies(packages, world, providers)
    hardlinks: dict[tuple[int, int, int], list[_Entry]] = defaultdict(list)
    for entry in entries:
        if stat.S_ISREG(entry.mode) and entry.links > 1:
            hardlinks[entry.inode].append(entry)
    files: dict[str, dict[str, Any]] = {}
    regular_bytes = symlink_bytes = page_bytes = unowned_bytes = shared_bytes = 0
    for entry in sorted(entries, key=lambda item: item.name):
        payload = entry.data
        linked = hardlinks.get(entry.inode, []) if entry.links > 1 else []
        if not stat.S_ISREG(entry.mode):
            linked = []
        links = sorted(item.name for item in linked)
        if linked:
            payload = next((item.data for item in linked if item.data), b"")
        counted = not links or entry.name == links[0]
        is_regular = stat.S_ISREG(entry.mode)
        is_symlink = stat.S_ISLNK(entry.mode)
        contribution = len(payload) if counted and (is_regular or is_symlink) else 0
        file_owners = sorted({owners[name] for name in links or [entry.name] if name in owners})
        files["/" + entry.name] = {
            "mode": entry.mode,
            "uid": entry.uid,
            "gid": entry.gid,
            "device": list(entry.device),
            "size": len(payload),
            "sha256": sha256_bytes(payload),
            "owners": file_owners,
            "hardlinks": ["/" + name for name in links],
            "accounted_bytes": contribution,
        }
        if is_regular:
            regular_bytes += contribution
            if counted:
                page_bytes += ((len(payload) + 4095) // 4096) * 4096
        elif is_symlink:
            symlink_bytes += contribution
        for owner in file_owners:
            packages[owner]["files"].append("/" + entry.name)
        if not file_owners:
            unowned_bytes += contribution
        elif len(file_owners) > 1:
            shared_bytes += contribution
        elif is_regular or is_symlink:
            key = "regular_payload_bytes" if is_regular else "symlink_payload_bytes"
            packages[file_owners[0]][key] += contribution
    return {
        "source": "external-root-composition" if external else "embedded-initramfs",
        "cpio_bytes": len(data),
        "cpio_sha256": sha256_bytes(data),
        "content_sha256": sha256_bytes(canonical_json_bytes(files)),
        "regular_payload_bytes": regular_bytes,
        "symlink_payload_bytes": symlink_bytes,
        "unowned_payload_bytes": unowned_bytes,
        "shared_owner_payload_bytes": shared_bytes,
        "file_page_model": {
            "page_bytes": 4096,
            "rounded_regular_bytes": page_bytes,
            "description": (
                "Each regular inode rounded to 4096 bytes; not RSS, PSS or MemAvailable"
            ),
        },
        "world": world,
        "packages": packages,
        "files": files,
    }, providers


def _ram_root_report(initramfs: bytes) -> dict[str, Any]:
    """Describe the compressed lower image that is actually embedded."""
    contents = {entry.name: entry.data for entry in _cpio_entries(initramfs)}
    lower = contents["root.squashfs"]
    if len(lower) < 96 or lower[:4] != b"hsqs":
        raise FootprintError("RAM root lower is not a SquashFS image")
    block_bytes = struct.unpack_from("<I", lower, 12)[0]
    compressor = struct.unpack_from("<H", lower, 20)[0]
    compression = {4: "xz", 5: "lz4"}.get(compressor)
    if compression is None:
        raise FootprintError("unsupported RAM root compression")
    return {
        "filesystem": "squashfs",
        "compression": compression,
        "block_bytes": block_bytes,
        "bytes": len(lower),
        "sha256": sha256_bytes(lower),
        "description": "Compressed RAM backing; file-cache pages are additional and reclaimable",
    }


def _elf_bytes_at(elf: bytes, address: int, size: int) -> bytes:
    if elf[:7] != b"\x7fELF\x01\x01\x01":
        raise FootprintError("kernel is not a little-endian ELF32 file")
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_size, section_count = struct.unpack_from("<HH", elf, 46)
    for index in range(section_count):
        section = struct.unpack_from("<10I", elf, section_offset + index * section_size)
        _, kind, _, start, offset, length, *_ = section
        if kind == 1 and start <= address and address + size <= start + length:
            result = elf[offset + address - start : offset + address - start + size]
            if len(result) == size:
                return result
    raise FootprintError("initramfs symbol lies outside the kernel file sections")


def _embedded_initramfs(elf: bytes, system_map: bytes, cpio: bytes) -> dict[str, Any]:
    symbols = {}
    for line in system_map.decode("ascii").splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in {"__initramfs_start", "__initramfs_size"}:
            symbols[fields[2]] = int(fields[0], 16)
    size = struct.unpack("<I", _elf_bytes_at(elf, symbols["__initramfs_size"], 4))[0]
    compressed = _elf_bytes_at(elf, symbols["__initramfs_start"], size)
    if compressed.startswith(b"\x1f\x8b"):
        compression, unpacked = "gzip", gzip.decompress(compressed)
    elif compressed.startswith(b"\xfd7zXZ\x00"):
        compression, unpacked = "xz", lzma.decompress(compressed)
    elif compressed.startswith(b"070701"):
        compression, unpacked = "none", compressed
    else:
        raise FootprintError("unsupported embedded initramfs compression")
    if unpacked != cpio:
        raise FootprintError("embedded initramfs differs from the published root filesystem")
    return {
        "compression": compression,
        "compressed_bytes": size,
        "cpio_bytes": len(unpacked),
        "sha256": sha256_bytes(compressed),
    }


def _apk_report(path: Path, providers: dict[str, list[str]]) -> dict[str, Any]:
    fields: dict[str, list[str]] = defaultdict(list)
    with tarfile.open(path, "r:gz", ignore_zeros=True) as archive:
        for member in archive:
            if member.name != ".PKGINFO":
                continue
            stream = archive.extractfile(member)
            if stream is None:
                raise FootprintError("optional APK has no package metadata")
            for line in stream.read().decode("utf-8").splitlines():
                key, separator, value = line.partition(" = ")
                if separator:
                    fields[key].append(value)
            break
    if not fields.get("pkgname") or not fields.get("pkgver"):
        raise FootprintError("optional APK has no package identity")
    return {
        "package": fields["pkgname"][0],
        "version": fields["pkgver"][0],
        "archive_bytes": path.stat().st_size,
        "dependencies": fields.get("depend", []),
        "provides": fields.get("provides", []),
        "base_dependency_edges": _dependency_edges(fields.get("depend", []), providers),
    }


def _optional_dependencies(
    optional: dict[str, dict[str, Any]],
    base_packages: dict[str, dict[str, Any]],
) -> None:
    packages = {package["package"]: package for package in optional.values()}
    providers = _providers(packages, {})
    for package in packages.values():
        package["optional_dependency_edges"] = _dependency_edges(
            package["dependencies"],
            providers,
        )
    for package in packages.values():
        queue = deque([package["package"]])
        seen: set[str] = set()
        while queue:
            name = queue.popleft()
            if name in seen:
                continue
            seen.add(name)
            if name in base_packages:
                edges = base_packages[name]["dependency_edges"]
            else:
                dependency = packages[name]
                edges = (
                    dependency["base_dependency_edges"] + dependency["optional_dependency_edges"]
                )
            for edge in edges:
                queue.extend(edge["providers"])
        package["preinstalled_dependency_packages"] = sorted(seen & base_packages.keys())


def _verified_files(bundle: CurrentBundle, manifest: dict[str, Any]) -> dict[str, Path]:
    files = {}
    for relative, record in manifest["files"].items():
        name = PurePosixPath(relative)
        if name.is_absolute() or ".." in name.parts:
            raise FootprintError("invalid footprint artifact path")
        path = bundle.path / relative
        if (
            path.is_symlink()
            or not path.is_file()
            or path.stat().st_size != record["size"]
            or sha256_file(path) != record["sha256"]
        ):
            raise FootprintError(f"bundle artifact is missing or changed: {relative}")
        files[relative] = path
    return files


def inspect_footprint(bundle: CurrentBundle) -> dict[str, Any]:
    """Read a resolved bundle without a build environment or device connection.

    File bytes are verified against the selected manifest. Nested size layers
    have no total; package contributions exclude dependencies' owned files.
    """
    try:
        manifest = json.loads(bundle.manifest_bytes)
        files = _verified_files(bundle, manifest)
        cpio = files["debug/rootfs.cpio"].read_bytes()
        config = files["debug/kernel.config"].read_text(encoding="utf-8")
        external = kconfig_values(config).get("CONFIG_INITRAMFS_SOURCE", '""') == '""'
        rootfs, providers = _rootfs_report(cpio, external=external)
        embedded = None
        ram_root = None
        if not external:
            initramfs = files["debug/initramfs.cpio"].read_bytes()
            ram_root = _ram_root_report(initramfs)
            rootfs["source"] = "ram-squashfs-composition"
            embedded = _embedded_initramfs(
                files["debug/vmlinux"].read_bytes(),
                files["debug/System.map"].read_bytes(),
                initramfs,
            )
        optional = {
            name: {**_apk_report(path, providers), "sha256": manifest["files"][name]["sha256"]}
            for name, path in files.items()
            if name.startswith("apks/") and name.endswith(".apk")
        }
        _optional_dependencies(optional, rootfs["packages"])
        boot = {
            name: manifest["files"][name]["size"]
            for name in files
            if name.startswith("image/") or name in manifest["boot_artifacts"]["required"]
        }
        return {
            "identity": {
                key: manifest.get(key) for key in ("target", "profile", "build_type", "generation")
            },
            "layers": {
                "boot_artifact_bytes": boot,
                "kernel_zimage_bytes": files["debug/zImage"].stat().st_size,
                "embedded_initramfs": embedded,
                "ram_root": ram_root,
                "host_debug_file_bytes": {
                    name: manifest["files"][name]["size"]
                    for name in files
                    if name.startswith("debug/")
                },
                "description": (
                    "Nested layers overlap; host debug copies are not added to loaded bytes"
                ),
            },
            "rootfs": rootfs,
            "optional_apks": optional,
        }
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        struct.error,
        tarfile.TarError,
        lzma.LZMAError,
    ) as error:
        raise FootprintError(f"cannot inspect bundle footprint: {error}") from error


def _mapping_delta(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    return {
        "added": {key: after[key] for key in sorted(after.keys() - before.keys())},
        "removed": {key: before[key] for key in sorted(before.keys() - after.keys())},
        "changed": {
            key: {"before": before[key], "after": after[key]}
            for key in sorted(before.keys() & after.keys())
            if before[key] != after[key]
        },
    }


def compare_footprints(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    """Compare saved measurements without opening bundles or rebuilding images."""
    old_root, new_root = before["rootfs"], after["rootfs"]
    old_layers, new_layers = before["layers"], after["layers"]
    old_initramfs = old_layers["embedded_initramfs"]
    new_initramfs = new_layers["embedded_initramfs"]
    old_boot, new_boot = old_layers["boot_artifact_bytes"], new_layers["boot_artifact_bytes"]
    return {
        "before": before["identity"],
        "after": after["identity"],
        "same_rootfs_content": old_root["content_sha256"] == new_root["content_sha256"],
        "layers": {"before": before["layers"], "after": after["layers"]},
        "kernel_zimage_byte_delta": (
            new_layers["kernel_zimage_bytes"] - old_layers["kernel_zimage_bytes"]
        ),
        "boot_artifact_byte_delta": {
            name: new_boot.get(name, 0) - old_boot.get(name, 0)
            for name in sorted(old_boot.keys() | new_boot.keys())
        },
        "initramfs_compression_changed": (
            old_initramfs["compression"] != new_initramfs["compression"]
            if old_initramfs is not None and new_initramfs is not None
            else None
        ),
        "rootfs_byte_delta": {
            key: new_root[key] - old_root[key]
            for key in ("cpio_bytes", "regular_payload_bytes", "symlink_payload_bytes")
        },
        "packages": _mapping_delta(old_root["packages"], new_root["packages"]),
        "files": _mapping_delta(old_root["files"], new_root["files"]),
        "optional_apks": _mapping_delta(before["optional_apks"], after["optional_apks"]),
    }

# SPDX-License-Identifier: GPL-2.0-only
"""Inspect published artifacts without building, extracting or executing them."""

from __future__ import annotations

import hashlib
import json
import re
import tarfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, cast

from fplinux_cli.artifacts.footprint import FootprintError, compare_footprints, inspect_footprint
from fplinux_cli.common import display_text, fail, sha256_file
from fplinux_cli.runtime.bundle_session import resolve_target_bundle

if TYPE_CHECKING:
    from collections.abc import Mapping


def _print_identity(manifest: Mapping[str, object]) -> None:
    """Show the identity recorded in the inspected build, not the current source checkout."""
    for key in ("target", "profile", "build_type", "generation"):
        value = manifest.get(key)
        if key == "profile" and value is None:
            value = "default"
        if not isinstance(value, str) or not value:
            fail(f"build manifest has no valid {key}")
        print(f"{key}: {value}")


def inspect_bundle(
    target: str, *, profile: str | None = None, build_type: str = "release"
) -> None:
    """Read the selected current generation and compare its files to its build manifest."""
    bundle, manifest = resolve_target_bundle(target, profile, build_type=build_type)
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        fail("build manifest has no file records")
    print(f"bundle: {display_text(bundle.path)}")
    _print_identity(manifest)
    print("SIZE SHA256 PATH")
    for relative, record in sorted(files.items()):
        artifact = bundle.path / relative
        size = artifact.stat().st_size
        digest = sha256_file(artifact)
        if (
            not isinstance(record, dict)
            or record.get("sha256") != digest
            or record.get("size") != size
        ):
            fail(f"bundle file differs from its build manifest: {relative}")
        print(f"{size} {digest} {relative}")
    print(f"files: {len(files)}; build-manifest checksums: OK")


def _byte_size(value: int) -> str:
    """Keep exact byte counts alongside a compact binary size for larger artifacts."""
    if abs(value) < 1024:
        return f"{value} B"
    return f"{value:,} B ({value / 1024:.1f} KiB)"


def inspect_target_footprint(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    json_output: bool = False,
) -> None:
    """Measure verified artifacts from the exact selected current bundle."""
    bundle, _manifest = resolve_target_bundle(target, profile, build_type=build_type)
    try:
        report = inspect_footprint(bundle)
    except FootprintError as error:
        fail(str(error))
    if json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
        return
    _print_identity(report["identity"])
    layers = report["layers"]
    print(layers["description"])
    print("Boot artifacts:")
    for name, size in sorted(layers["boot_artifact_bytes"].items()):
        print(f"  {_byte_size(size)}  {name}")
    print(f"Kernel zImage: {_byte_size(layers['kernel_zimage_bytes'])}")
    embedded = layers["embedded_initramfs"]
    if embedded is not None:
        print(
            f"Embedded initramfs ({embedded['compression']}): "
            f"{_byte_size(embedded['compressed_bytes'])}"
        )
    rootfs = report["rootfs"]
    print(f"Root filesystem ({rootfs['source']}):")
    print(f"  cpio: {_byte_size(rootfs['cpio_bytes'])}")
    print(f"  regular payload: {_byte_size(rootfs['regular_payload_bytes'])}")
    print(f"  symlink payload: {_byte_size(rootfs['symlink_payload_bytes'])}")
    print("Packages (owned regular and symlink payload; dependencies excluded):")
    for name, package in sorted(rootfs["packages"].items()):
        size = package["regular_payload_bytes"] + package["symlink_payload_bytes"]
        print(f"  {_byte_size(size)}  {name} {package['version']}")
    print("Optional APK archives:")
    for name, package in sorted(report["optional_apks"].items()):
        print(f"  {_byte_size(package['archive_bytes'])}  {name}")
    print("Host debug files:")
    for name, size in sorted(layers["host_debug_file_bytes"].items()):
        print(f"  {_byte_size(size)}  {name}")


def inspect_footprint_diff(before: Path, after: Path, *, json_output: bool = False) -> None:
    """Compare saved measurements without resolving a bundle or touching the cache."""
    try:
        old = json.loads(before.read_text(encoding="utf-8"))
        new = json.loads(after.read_text(encoding="utf-8"))
        if not isinstance(old, dict) or not isinstance(new, dict):
            fail("footprint reports must be JSON objects")
        report = compare_footprints(old, new)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        fail(
            f"cannot compare footprint reports {display_text(before)} and "
            f"{display_text(after)}: {error}"
        )
    if json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
        return
    print(f"before: {display_text(before)}")
    print(f"after: {display_text(after)}")
    print(f"same rootfs content: {'yes' if report['same_rootfs_content'] else 'no'}")
    print(f"Kernel zImage delta: {_byte_size(report['kernel_zimage_byte_delta'])}")
    print("Boot artifact deltas:")
    for name, delta in report["boot_artifact_byte_delta"].items():
        print(f"  {_byte_size(delta)}  {name}")
    print("Root filesystem deltas:")
    for name, delta in report["rootfs_byte_delta"].items():
        print(f"  {_byte_size(delta)}  {name}")
    for kind in ("packages", "files", "optional_apks"):
        print(f"{kind}:")
        for change, entries in report[kind].items():
            print(f"  {change}: {', '.join(entries) or 'none'}")


def _archive_checksums(archive: zipfile.ZipFile) -> tuple[str, dict[str, str]]:
    """Read the checksum list emitted by the FPLinux packager."""
    lists = [
        item for item in archive.infolist() if PurePosixPath(item.filename).name == "SHA256SUMS"
    ]
    if len(lists) != 1:
        fail("archive must contain one SHA256SUMS file")
    prefix = lists[0].filename.removesuffix("SHA256SUMS")
    checksums: dict[str, str] = {}
    for line in archive.read(lists[0]).decode("utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            fail("archive has an invalid SHA256SUMS entry")
        digest, name = match.groups()
        if name in checksums:
            fail(f"archive has a duplicate checksum entry: {name}")
        checksums[name] = digest
    return prefix, checksums


def inspect_archive(path: Path) -> None:
    """Check a candidate or release ZIP against its complete internal SHA256SUMS list."""
    try:
        with zipfile.ZipFile(path) as archive:
            prefix, checksums = _archive_checksums(archive)
            entries = [item for item in archive.infolist() if not item.is_dir()]
            files = {item.filename: item for item in entries}
            expected = {prefix + name for name in checksums} | {prefix + "SHA256SUMS"}
            if len(files) != len(entries) or set(files) != expected:
                fail("archive files do not match its SHA256SUMS inventory")
            for name, expected_digest in checksums.items():
                with archive.open(files[prefix + name]) as stream:
                    digest = hashlib.file_digest(
                        cast("zipfile.ZipExtFile", stream), "sha256"
                    ).hexdigest()
                if digest != expected_digest:
                    fail(f"archive checksum mismatch: {name}")
            manifest = json.loads(archive.read(prefix + "build-manifest.json"))
            if not isinstance(manifest, dict):
                fail("archive build manifest must be an object")
            print(f"archive: {display_text(path)}")
            kind = "candidate" if prefix + "CANDIDATE-NOTICE.txt" in files else "release"
            print(f"kind: {kind}")
            _print_identity(manifest)
            print("SIZE SHA256 PATH")
            for name, digest in sorted(checksums.items()):
                print(f"{files[prefix + name].file_size} {digest} {name}")
            print(f"files: {len(checksums)}; SHA256SUMS: OK")
    except (zipfile.BadZipFile, KeyError, ValueError) as error:
        fail(f"cannot inspect archive {display_text(path)}: {error}")


def inspect_apk(path: Path) -> None:
    """Read APK v2 control and data tar members, without validating package signatures."""
    try:
        # APK v2 joins gzip/tar streams for signature, control and data sections.
        with tarfile.open(path, "r:gz", ignore_zeros=True) as archive:
            metadata = archive.extractfile(".PKGINFO")
            if metadata is None:
                fail("APK has no regular .PKGINFO file")
            with metadata:
                text = metadata.read().decode("utf-8")
            print(f"apk: {display_text(path)}")
            print("metadata (.PKGINFO):")
            print(text, end="" if text.endswith("\n") else "\n")
            print("TYPE SIZE PATH")
            for member in archive:
                if member.isdir():
                    kind = "dir"
                elif member.issym():
                    kind = "symlink"
                elif member.islnk():
                    kind = "hardlink"
                else:
                    kind = "file" if member.isfile() else "special"
                suffix = f" -> {member.linkname}" if member.issym() or member.islnk() else ""
                print(f"{kind} {member.size} {member.name}{suffix}")
    except (tarfile.TarError, KeyError, UnicodeDecodeError, EOFError) as error:
        fail(f"cannot inspect APK {display_text(path)}: {error}")

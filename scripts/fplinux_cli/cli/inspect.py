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

from fplinux_cli.common import display_text, fail, sha256_file

from .bundles import resolve_target_bundle

if TYPE_CHECKING:
    from collections.abc import Mapping


def _print_identity(manifest: Mapping[str, object]) -> None:
    """Show the identity recorded in the inspected build, not the current source checkout."""
    for key in ("target", "profile", "generation"):
        value = manifest.get(key)
        if key == "profile" and value is None:
            value = "default"
        if not isinstance(value, str) or not value:
            fail(f"build manifest has no valid {key}")
        print(f"{key}: {value}")


def inspect_bundle(target: str, *, profile: str | None = None) -> None:
    """Read the selected current generation and compare its files to its build manifest."""
    bundle, manifest = resolve_target_bundle(target, profile)
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

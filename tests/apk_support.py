# SPDX-License-Identifier: GPL-2.0-only
"""Independent signed APK v2 fixtures for native package transactions."""

from __future__ import annotations

import gzip
import hashlib
import io
import subprocess
import tarfile
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class ApkIdentity:
    """Declare only the package constraints exercised by the signed fixtures."""

    name: str
    arch: str
    version: str = "1.0-r0"
    provides: tuple[str, ...] = ()
    depends: tuple[str, ...] = ()
    replaces: tuple[str, ...] = ()


def _apk_tar(files: dict[str, bytes], *, segment: bool = False) -> bytes:
    """Build the tar streams of the documented APK v2 fixture format."""
    buffer = io.BytesIO()
    directories = {
        str(parent)
        for name in files
        for parent in PurePosixPath(name).parents
        if str(parent) != "."
    }
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for name in sorted(directories):
            member = tarfile.TarInfo(name)
            member.type = tarfile.DIRTYPE
            member.mode = 0o755
            archive.addfile(member)
        for name, contents in files.items():
            member = tarfile.TarInfo(name)
            member.mode = 0o644
            member.size = len(contents)
            if not segment:
                member.pax_headers = {
                    "APK-TOOLS.checksum.SHA1": hashlib.sha1(
                        contents, usedforsecurity=False
                    ).hexdigest()
                }
            archive.addfile(member, io.BytesIO(contents))
        segment_size = archive.offset
    contents = buffer.getvalue()
    # Control and signature segments omit tar end markers; the data stream keeps them.
    if segment:
        contents = contents[:segment_size]
    return gzip.compress(contents, mtime=0)


def _apk_tree(root: Path) -> bytes:
    """Retain the actual staged files, modes, directories and symbolic links."""
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for source in sorted(root.rglob("*")):
            member = archive.gettarinfo(str(source), arcname=source.relative_to(root).as_posix())
            member.uid = member.gid = member.mtime = 0
            member.uname = member.gname = ""
            if member.isfile():
                contents = source.read_bytes()
                member.pax_headers = {
                    "APK-TOOLS.checksum.SHA1": hashlib.sha1(
                        contents, usedforsecurity=False
                    ).hexdigest()
                }
                archive.addfile(member, io.BytesIO(contents))
            else:
                archive.addfile(member)
    return gzip.compress(buffer.getvalue(), mtime=0)


def _sign_apk(directory: Path, identity: ApkIdentity, data: bytes, private_key: Path) -> Path:
    """Sign a declared APK v2 payload without using production metadata logic."""
    # https://wiki.alpinelinux.org/wiki/Apk_spec defines the three gzip streams.
    metadata = (
        f"pkgname = {identity.name}\n"
        f"pkgver = {identity.version}\n"
        "pkgdesc = Isolated package transaction fixture\n"
        "url = https://example.invalid\n"
        "builddate = 0\n"
        "size = 4096\n"
        f"arch = {identity.arch}\n"
        "license = GPL-2.0-only\n"
        f"datahash = {hashlib.sha256(data).hexdigest()}\n"
    )
    for field, values in (
        ("provides", identity.provides),
        ("depend", identity.depends),
        ("replaces", identity.replaces),
    ):
        for value in values:
            metadata += f"{field} = {value}\n"
    control = _apk_tar({".PKGINFO": metadata.encode()}, segment=True)
    signature = subprocess.run(
        ["openssl", "dgst", "-sha1", "-sign", str(private_key)],
        input=control,
        capture_output=True,
        check=True,
        timeout=10,
    ).stdout
    signature_stream = _apk_tar({".SIGN.RSA.sysroot-test.rsa.pub": signature}, segment=True)
    package = directory / f"{identity.name}-{identity.version}.apk"
    package.write_bytes(signature_stream + control + data)
    return package


def signed_apk(  # noqa: PLR0913 -- content, architecture and signing inputs stay explicit.
    directory: Path,
    name: str,
    files: dict[str, bytes],
    *,
    arch: str,
    private_key: Path,
    replacement: str | None = None,
) -> Path:
    """Create a signed regular-file fixture with the original replacement contract."""
    identity = ApkIdentity(
        name=name,
        arch=arch,
        provides=() if replacement is None else (f"{replacement}=1.0-r0",),
        replaces=() if replacement is None else (replacement,),
    )
    return _sign_apk(directory, identity, _apk_tar(files), private_key)


def signed_apk_tree(
    directory: Path,
    identity: ApkIdentity,
    root: Path,
    *,
    private_key: Path,
) -> Path:
    """Sign a recipe-produced tree, preserving its payload and file ownership paths."""
    return _sign_apk(directory, identity, _apk_tree(root), private_key)

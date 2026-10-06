# SPDX-License-Identifier: GPL-2.0-only
"""Stand in for abuild while using real source files, modes and an APK archive."""

from __future__ import annotations

import io
import sys
import tarfile
from pathlib import Path

sources = Path(sys.argv[1])
sources.chmod(0o700)
assert (sources / "existing.tar.xz").read_bytes() == b"existing source\n"
payload = sources / "downloaded.tar.xz"
payload.write_bytes(b"new source\n")
payload.chmod(0o600)
repository = Path(sys.argv[2])
metadata = b"pkgname = fplinux-xkb-ru\n"
with tarfile.open(repository / "fplinux-xkb-ru-2.48-r0.apk", "w:gz") as archive:
    member = tarfile.TarInfo(".PKGINFO")
    member.size = len(metadata)
    archive.addfile(member, io.BytesIO(metadata))

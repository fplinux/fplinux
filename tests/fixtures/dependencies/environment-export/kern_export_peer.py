#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Provide controlled metadata and a native single-layer image export."""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
from pathlib import Path


def add_member(bundle: tarfile.TarFile, name: str, payload: bytes) -> None:
    """Write a regular member with visible metadata for the archive consumer."""
    member = tarfile.TarInfo(name)
    member.size = len(payload)
    member.mode = 0o640
    member.uid = 1000
    member.gid = 1001
    member.uname = "builder"
    member.gname = "builders"
    member.mtime = 1712345678
    bundle.addfile(member, io.BytesIO(payload))


def export_image(reference: str, destination: Path) -> None:
    """Write the uncompressed layer and its content-derived config and manifest."""
    layer_buffer = io.BytesIO()
    with tarfile.open(fileobj=layer_buffer, mode="w", format=tarfile.GNU_FORMAT) as layer:
        add_member(layer, "./usr/tool", b"installed tool payload\n" * 4096)
        link = tarfile.TarInfo("./usr/tool-link")
        link.type = tarfile.SYMTYPE
        link.linkname = "tool"
        link.mode = 0o777
        link.uid = 123
        link.gid = 456
        link.mtime = 1700000000
        layer.addfile(link)
    layer_bytes = layer_buffer.getvalue()
    layer_id = hashlib.sha256(layer_bytes).hexdigest()
    config = json.dumps(
        {
            "architecture": "amd64",
            "os": "linux",
            "config": {"Env": ["LANG=C"], "Cmd": ["/bin/sh"], "User": "1000"},
            "rootfs": {"type": "layers", "diff_ids": ["sha256:" + layer_id]},
        },
        separators=(",", ":"),
    ).encode()
    config_name = hashlib.sha256(config).hexdigest() + ".json"
    layer_name = layer_id + "/layer.tar"
    manifest = json.dumps(
        [{"Config": config_name, "RepoTags": [reference], "Layers": [layer_name]}],
        separators=(",", ":"),
    ).encode()
    with tarfile.open(destination, "w", format=tarfile.GNU_FORMAT) as archive:
        directory = tarfile.TarInfo("./" + layer_id)
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o750
        directory.uid = 1000
        directory.gid = 1001
        directory.mtime = 1712345678
        archive.addfile(directory)
        add_member(archive, "./" + layer_name, layer_bytes)
        add_member(archive, "./" + config_name, config)
        add_member(archive, "./manifest.json", manifest)
    shutil.copyfile(destination, Path(os.environ["FPLINUX_TEST_ORIGINAL_EXPORT"]))


def main() -> None:
    """Replace the metadata and save process boundaries without an image store."""
    arguments = sys.argv[1:]
    if arguments[0] == "box" and arguments[-1] == "--metadata":
        print('[{"gid":0,"mode":493,"path":"usr","type":"directory","uid":1000}]')
    elif len(arguments) == 4 and arguments[0] == "save" and arguments[2] == "-o":
        export_image(arguments[1], Path(arguments[3]))
    else:
        message = f"unexpected image-runtime command: {arguments!r}"
        raise SystemExit(message)


if __name__ == "__main__":
    main()

# SPDX-License-Identifier: GPL-2.0-only
"""Independent ZIP and dependency-transport fixtures for release boundary tests."""

from __future__ import annotations

import hashlib
import json
import zipfile
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from pathlib import Path

COMMIT = "e" * 40
RELEASE_ID = "eeeeeeeeeeee"
SELECTIONS = (
    ("bq-2830l-tank-connect-max", "linux"),
    ("bq-2830l-tank-connect-max", "windows"),
    ("inoi-240-modern-4g", "linux"),
    ("inoi-240-modern-4g", "windows"),
    ("inoi-244-modern-4g", "linux"),
    ("inoi-244-modern-4g", "windows"),
    ("maxvi-k15n-4g", "linux"),
    ("maxvi-k15n-4g", "windows"),
    ("nokia-110-4g", "linux"),
    ("nokia-110-4g", "windows"),
    ("nokia-ta1618", "linux"),
    ("nokia-ta1618", "windows"),
    ("uniwa-v1000-4g", "linux"),
    ("uniwa-v1000-4g", "windows"),
    ("xiaomi-qin-1s-plus", "linux"),
    ("xiaomi-qin-1s-plus", "windows"),
)


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def project_sources() -> dict[str, bytes]:
    return {
        "project/common/example.c": b"one shared project source\n",
        "project/fplinux": b"#!/bin/sh\nexit 0\n",
        "project/LICENSE": b"project license\n",
        "project/THIRD_PARTY_NOTICES.md": b"third-party notices\n",
        **{
            f"project/targets/{target}/target.toml": b'platform = "test"\n'
            for target, _host in SELECTIONS
        },
    }


def release_fields() -> dict[str, str]:
    records = [
        {
            "path": name.removeprefix("project/"),
            "sha256": hashlib.sha256(contents).hexdigest(),
            "mode": 0o755 if name == "project/fplinux" else 0o644,
        }
        for name, contents in sorted(project_sources().items())
    ]
    return {
        "release_id": RELEASE_ID,
        "source_commit": COMMIT,
        "source_snapshot": hashlib.sha256(json_bytes(records)).hexdigest(),
    }


def write_zip(path: Path, files: dict[str, bytes], executables: set[str]) -> None:
    sums = b"".join(
        f"{hashlib.sha256(contents).hexdigest()}  {name}\n".encode()
        for name, contents in sorted(files.items())
    )
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, contents in sorted({**files, "SHA256SUMS": sums}.items()):
            member = zipfile.ZipInfo(f"{path.stem}/{name}")
            member.create_system = 3
            member.external_attr = (0o100755 if name in executables else 0o100644) << 16
            archive.writestr(member, contents)


def read_zip(path: Path) -> tuple[dict[str, bytes], dict[str, int]]:
    with zipfile.ZipFile(path) as archive:
        files = {
            item.filename.removeprefix(f"{path.stem}/"): archive.read(item)
            for item in archive.infolist()
        }
        modes = {
            item.filename.removeprefix(f"{path.stem}/"): item.external_attr >> 16
            for item in archive.infolist()
        }
    return files, modes


def write_pair(  # noqa: PLR0913 -- each option selects a distinct archive-boundary scenario.
    directory: Path,
    target: str,
    host: str,
    *,
    private_runtime: bool = False,
    private_member: str | None = None,
    mismatched_pair: bool = False,
    conflicting_source: bool = False,
) -> None:
    stem = f"FPLinux-{target}-{host}-x64-{RELEASE_ID}"
    files = {
        "README.txt": (
            "Run the archive.\n\nMatching source and build material:\n"
            f"  {stem}-sources.zip\n"
            "Keep it available alongside this archive. Licenses remain included here.\n"
        ).encode(),
        "image/ramboot.bin": b"small synthetic image",
        "runtime-manifest.json": json_bytes({"target": target, "sha256": {}}),
        "Run.cmd" if host == "windows" else "Run.sh": b"launcher\n",
    }
    if private_runtime:
        files["device-data.cpio"] = b"prepared personal phone data"
    if private_member is not None:
        files[private_member] = b"private installation state"
    executables = {"Run.sh"} if host == "linux" else set()
    manifest: dict[str, Any] = {
        **release_fields(),
        "target": target,
        "profile": None,
        "build_type": "release",
        "device_identity": "f" * 64,
        "files": {
            name: {
                "sha256": hashlib.sha256(data).hexdigest(),
                "mode": 0o755 if name in executables else 0o644,
                "size": len(data),
            }
            for name, data in sorted(files.items())
        },
    }
    manifest["generation"] = hashlib.sha256(json_bytes(manifest)).hexdigest()
    files["build-manifest.json"] = json_bytes(manifest)
    runtime = directory / f"{stem}.zip"
    write_zip(runtime, files, executables)
    sources = {
        **project_sources(),
        "project/.fplinux-release.json": json_bytes(release_fields()),
        "upstream/shared.tar.xz": b"same exact upstream input",
        "upstream/downloads.json": json_bytes(
            [
                {
                    "archive": "upstream/shared.tar.xz",
                    "algorithm": "sha256",
                    "checksum": hashlib.sha256(b"same exact upstream input").hexdigest(),
                }
            ]
        ),
        "upstream/alpine-source-channels.json": b"[]\n",
        "upstream/windows-runtime-source-channels.json": b"Windows source channels\n",
        "sources/host-tools.zip": f"source projection for {target}/{host}\n".encode(),
        "licenses/host-tools/COPYING": b"retained license\n",
        "BUILD-LINUX.txt": b"retained relinking and build instructions\n",
        "DISTRIBUTION.json": json_bytes(
            {
                **release_fields(),
                "runtimes": [
                    {
                        "archive": runtime.name,
                        "target": target,
                        "host": host,
                        "identity": "f" * 64,
                        "sha256": "0" * 64
                        if mismatched_pair
                        else hashlib.sha256(runtime.read_bytes()).hexdigest(),
                    }
                ],
            }
        ),
    }
    if conflicting_source:
        sources["project/common/example.c"] = b"conflicting source\n"
    if host == "windows":
        sources.update(
            {
                "sources/windows-tools-source.zip": b"source projection\n",
                "sources/windows-driver/README.txt": b"driver build instructions\n",
                "sources/windows-driver/build.py": b"driver build program\n",
                "sources/windows-driver/downloads.json": b"{}\n",
            }
        )
    write_zip(directory / f"{stem}-sources.zip", sources, {"project/fplinux"})


def write_transport(directory: Path) -> None:
    directory.mkdir()
    part = f"FPLinux-{RELEASE_ID}-inputs.tar.zst.part-00000"
    data = b"retained transport original bytes\n"
    (directory / part).write_bytes(data)
    index = f"FPLinux-{RELEASE_ID}-inputs.sha256"
    contents = f"{hashlib.sha256(data).hexdigest()}  {part}\n".encode()
    (directory / index).write_bytes(contents)
    (directory / "DEPENDENCIES.json").write_bytes(
        json_bytes(
            {
                **release_fields(),
                "snapshot": "1" * 64,
                "index": index,
                "index_sha256": hashlib.sha256(contents).hexdigest(),
            }
        )
    )

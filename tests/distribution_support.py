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
CARD_TARGETS = frozenset(
    {
        "bq-2830l-tank-connect-max",
        "itel-neo-r60-plus",
        "maxvi-k15n-4g",
        "inoi-240-modern-4g",
        "inoi-244-modern-4g",
        "nokia-110-4g",
        "nokia-ta1618",
        "uniwa-v1000-4g",
    }
)
SELECTIONS = (
    ("bq-2830l-tank-connect-max", "linux"),
    ("bq-2830l-tank-connect-max", "windows"),
    ("inoi-240-modern-4g", "linux"),
    ("inoi-240-modern-4g", "windows"),
    ("inoi-244-modern-4g", "linux"),
    ("inoi-244-modern-4g", "windows"),
    ("itel-neo-r60-plus", "linux"),
    ("itel-neo-r60-plus", "windows"),
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
        "project/VERSION": b"1.0.0\n",
        "project/targets/konrow-senior-280-plus/target.toml": b'platform = "test"\n',
        **{
            f"project/targets/{target}/target.toml": b'platform = "test"\n'
            for target, _host in SELECTIONS
        },
    }


def release_fields(commit: str = COMMIT) -> dict[str, str]:
    records = [
        {
            "path": name.removeprefix("project/"),
            "sha256": hashlib.sha256(contents).hexdigest(),
            "mode": 0o755 if name == "project/fplinux" else 0o644,
        }
        for name, contents in sorted(project_sources().items())
    ]
    return {
        "release_id": commit[:12],
        "source_commit": commit,
        "release_version": "1.0.0",
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


def _manifest(
    files: dict[str, bytes], modes: set[str], target: str, profile: str | None, commit: str
) -> bytes:
    body: dict[str, Any] = {
        **release_fields(commit),
        "target": target,
        "profile": profile,
        "build_type": "release",
        "device_identity": ("f" if profile is None else "a") * 64,
        "files": {
            name: {
                "sha256": hashlib.sha256(data).hexdigest(),
                "mode": 0o755 if name in modes else 0o644,
                "size": len(data),
            }
            for name, data in sorted(files.items())
        },
    }
    body["generation"] = hashlib.sha256(json_bytes(body)).hexdigest()
    return json_bytes(body)


def bundle_fixture(
    target: str,
    host: str,
    *,
    card: bool,
    profile: str | None = None,
    commit: str = COMMIT,
) -> tuple[dict[str, bytes], set[str]]:
    platform = "sc9820e" if target == "xiaomi-qin-1s-plus" else "ums9117"
    head = f"board bootstrap for {target}\n".encode()
    kernel = f"kernel for {platform}\n".encode()
    root = f"hsqs: shared system for {platform}\n".encode()
    runtime = {
        **release_fields(commit),
        "target": target,
        "profile": profile,
        "image": "image/ramboot.bin",
        "identity": {"target": {"display_name": target}, "platform": {"name": platform}},
        "sha256": {},
    }
    files = {
        "image/ramboot.bin": head + kernel + root
        if profile is None
        else b"USB stage0 with U-Boot",
        "assets.lock.toml": f'target = "{target}"\n'.encode(),
        "licenses/host-tools/COPYING": b"included host tool license\n",
        "runtime-manifest.json": json_bytes(runtime),
    }
    if profile is None:
        files["boot-components.json"] = json_bytes(
            {
                "platform": platform,
                "image": "image/ramboot.bin",
                "kernel": [len(head), len(kernel)],
                "root": [len(head) + len(kernel), len(root)],
            }
        )
        if card:
            files.update(
                {
                    "card/layout.json": json_bytes({"release_id": commit[:12], "target": target}),
                    "card/boot.xz": f"card boot for {target}\n".encode(),
                    "card/root.ext4.xz": b"common card system",
                    "card/board.tar": f"card system files for {target}\n".encode(),
                    "card/apk-state.tar": f"installed package state for {target}\n".encode(),
                    "card/fit.itb": head + kernel + b"card FIT trailer",
                    "card/apks/Packages.adb": b"admitted card package index",
                    "card/apks/fplinux-build.rsa.pub": b"admitted card package public key",
                    "card/apks/fplinux-notes.apk": b"admitted card profile notes package",
                }
            )
    modes: set[str] = set()
    if host == "linux":
        files["runtime/bin/python3.15"] = b"bundled executable\n"
        modes.add("runtime/bin/python3.15")
    files["build-manifest.json"] = _manifest(files, modes, target, profile, commit)
    files["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(files.items())
    ).encode()
    return files, modes


def write_release(
    directory: Path, *, selections: tuple[tuple[str, str], ...] = SELECTIONS, commit: str = COMMIT
) -> None:
    """Create a small release via the real ZIP writer using literal input bytes."""
    from fplinux_cli.artifacts.release_identity import release_context_from_fields  # noqa: PLC0415
    from fplinux_cli.packaging.distribution import _write_distribution  # noqa: PLC0415
    from fplinux_cli.packaging.sources import SourcePayload  # noqa: PLC0415
    from fplinux_cli.packaging.universal import HostArchive  # noqa: PLC0415

    sources = {
        **project_sources(),
        "project/.fplinux-release.json": json_bytes(release_fields(commit)),
        "upstream/shared.tar.xz": b"same exact upstream input",
        "upstream/windows-runtime-source-channels.json": b"Windows source channels\n",
        "licenses/host-tools/COPYING": b"retained license\n",
        "BUILD-LINUX.txt": b"retained relinking and build instructions\n",
    }
    archives: dict[str, HostArchive] = {}
    release = release_context_from_fields(release_fields(commit))
    identities = {}
    for target, host in selections:
        payload = archives.setdefault(host, HostArchive(host, release))
        card = target in CARD_TARGETS
        files, modes = bundle_fixture(target, host, card=card, commit=commit)
        microsd = (
            bundle_fixture(target, host, card=False, profile="microsd-uboot", commit=commit)
            if card
            else None
        )
        payload.add(target, files, modes, microsd=microsd)
        identities[target, host] = "f" * 64
        selection = f"selections/{target}/{host}"
        sources.update(
            {
                f"{selection}/upstream/downloads.json": json_bytes(
                    [
                        {
                            "archive": "upstream/shared.tar.xz",
                            "algorithm": "sha256",
                            "checksum": hashlib.sha256(b"same exact upstream input").hexdigest(),
                        }
                    ]
                ),
                f"{selection}/upstream/alpine-source-channels.json": b"[]\n",
                f"{selection}/sources/host-tools.zip": (
                    f"source projection for {target}/{host}\n".encode()
                ),
            }
        )
        if host == "windows":
            sources.update(
                {
                    f"{selection}/sources/windows-tools-source.zip": b"source projection\n",
                    f"{selection}/sources/windows-driver/README.txt": b"driver instructions\n",
                    f"{selection}/sources/windows-driver/build.py": b"driver build program\n",
                    f"{selection}/sources/windows-driver/downloads.json": b"{}\n",
                }
            )
    for archive in archives.values():
        archive.finish(f"FPLinux-{commit[:12]}-sources")
    _write_distribution(
        directory,
        release,
        archives,
        identities,
        SourcePayload(sources, {"project/fplinux"}, {}),
    )


def write_transport(directory: Path, *, commit: str = COMMIT) -> None:
    directory.mkdir()
    part = f"FPLinux-{commit[:12]}-inputs.tar.zst.part-00000"
    data = b"retained transport original bytes\n"
    (directory / part).write_bytes(data)
    index = f"FPLinux-{commit[:12]}-inputs.sha256"
    contents = f"{hashlib.sha256(data).hexdigest()}  {part}\n".encode()
    (directory / index).write_bytes(contents)
    (directory / "DEPENDENCIES.json").write_bytes(
        json_bytes(
            {
                **release_fields(commit),
                "snapshot": "1" * 64,
                "index": index,
                "index_sha256": hashlib.sha256(contents).hexdigest(),
            }
        )
    )

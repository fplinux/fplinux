# SPDX-License-Identifier: GPL-2.0-only
"""Inspect selected build bytes and their notices in the produced release ZIP."""

from __future__ import annotations

import hashlib
import json
from dataclasses import replace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.artifacts.release_identity import ReleaseContext
from fplinux_cli.packaging import distribution

from tests.distribution_support import read_zip
from tests.host_process.packaging.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.host_process.packaging.autonomous_package_fixtures import ArchiveInputs


@pytest.mark.parametrize("host", ["linux", "windows"])
def test_release_preserves_optional_apps_documents_and_native_file_modes(
    archive_inputs: ArchiveInputs, host: str
) -> None:
    inputs = archive_inputs
    files, modes = read_zip(inputs.package(host))
    board = json.loads(files["targets.json"])["targets"][inputs.target]
    payload = {
        name: b"".join(files[part] for part in parts) for name, parts in board["files"].items()
    }
    assert payload["EXTRA.txt"] == b"phone instructions\n"
    assert payload["LICENSE"] == (inputs.root / "LICENSE").read_bytes()
    assert (
        payload["licenses/musl/COPYRIGHT"]
        == (inputs.root / "third-party-licenses/musl/COPYRIGHT").read_bytes()
    )
    assert (
        payload["apks/fplinux-phoneme.apk"] == b"synthetic built file: apks/fplinux-phoneme.apk\n"
    )
    assert (
        payload["apks/fixture-runtime.apk"] == b"synthetic built file: apks/fixture-runtime.apk\n"
    )
    assert payload["image/ramboot.bin"] == (inputs.bundle / "image/ramboot.bin").read_bytes()
    assert "device-data.cpio" not in payload
    assert b"BUILDER PERSONAL DATA MUST STAY PRIVATE" not in b"".join(files.values())
    assert "host/fplinux-usb-keyboard" not in payload
    assert "fplinux.py" in board["executables"]
    assert modes["fplinux.py"] == 0o100755
    assert modes["README.txt"] == 0o100644
    if host == "linux":
        assert board["executables"] == [
            "fplinux.py",
            "host/libc_server",
            "host/spd_dump",
            "runtime/bin/python3.15",
            "runtime/bin/sftp",
            "runtime/lib/ld-musl-x86_64.so.1",
            "tools/linux/sftp",
        ]
        assert modes["Run.sh"] == 0o100755
        assert modes["runtime/bin/python3.15"] == 0o100755
        assert (
            payload["licenses/host-tools/Unlicense.txt"]
            == (inputs.root / "LICENSES/Unlicense.txt").read_bytes()
        )
    else:
        assert board["executables"] == [
            "fplinux.py",
            "runtime/python.exe",
            "tools/driver/wdi-simple.exe",
            "tools/libc_server.exe",
            "tools/openssh/ssh-keygen.exe",
            "tools/openssh/ssh-keyscan.exe",
            "tools/openssh/ssh.exe",
            "tools/spd_dump.exe",
        ]
        assert modes["Run.cmd"] == 0o100644
        assert modes["runtime/python.exe"] == 0o100755


def test_linux_release_includes_sftp_in_the_shared_runtime_and_manifests(
    archive_inputs: ArchiveInputs,
) -> None:
    files, modes = read_zip(archive_inputs.package("linux"))
    board = json.loads(files["targets.json"])["targets"][archive_inputs.target]
    build = json.loads(files[board["files"]["build-manifest.json"][0]])
    runtime = json.loads(files[board["files"]["runtime-manifest.json"][0]])
    for relative in ("runtime/bin/sftp", "tools/linux/sftp"):
        data = archive_inputs.linux_files[relative].read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        assert board["files"][relative] == [relative]
        assert files[relative] == data
        assert modes[relative] == 0o100755
        assert build["files"][relative] == {"mode": 0o755, "sha256": digest, "size": len(data)}
        assert runtime["sha256"][relative] == digest


@pytest.mark.parametrize("script", ["workflow.py", "card-slots.sh"])
def test_release_uses_the_runtime_from_the_admitted_build(
    archive_inputs: ArchiveInputs, script: str
) -> None:
    helper = f"scripts/fplinux_cli/runtime/{script}"
    original = (archive_inputs.bundle / helper).read_bytes()
    (archive_inputs.root / helper).write_bytes(b"changed checkout runtime\n")

    files, _modes = read_zip(archive_inputs.package("linux"))

    assert files[helper] == original


def test_release_rejects_mismatched_metadata_for_an_admitted_file(
    archive_inputs: ArchiveInputs,
) -> None:
    archive_inputs.manifest["files"]["image/ramboot.bin"]["size"] = 1

    with pytest.raises(SystemExit, match="release input differs"):
        archive_inputs.package("linux")

    assert not archive_inputs.output.exists()


def test_release_preserves_existing_archive_when_native_bytes_change(
    archive_inputs: ArchiveInputs,
) -> None:
    archive = archive_inputs.package("linux")
    original = archive.read_bytes()
    archive_inputs.linux_files["runtime/bin/python3.15"].write_bytes(b"changed dependency")

    with pytest.raises(SystemExit, match="output directory contains a different release"):
        archive_inputs.package("linux")

    assert archive.read_bytes() == original


def test_release_rejects_changed_source_context_before_publication(
    archive_inputs: ArchiveInputs, monkeypatch: pytest.MonkeyPatch
) -> None:
    initial = archive_inputs.snapshot
    changed = replace(initial, release=ReleaseContext("b" * 12, "1.0.0", None, "b" * 64))
    snapshots = iter((initial, changed))
    monkeypatch.setattr(distribution, "capture_public_sources", lambda **_options: next(snapshots))

    with pytest.raises(SystemExit, match="release sources changed"):
        archive_inputs.package("linux")

    assert not archive_inputs.output.exists()


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
@pytest.mark.parametrize("host", ["linux", "windows"])
def test_release_carries_each_qin_loader_and_its_original_notices(
    archive_inputs: ArchiveInputs, host: str
) -> None:
    files, _modes = read_zip(archive_inputs.package(host))
    payload = json.loads(files["targets.json"])["targets"][archive_inputs.target]["files"]
    for name in ("assets/fdl1.bin", "assets/fdl2.bin", "assets/sml.bin"):
        contents = (archive_inputs.bundle / name).read_bytes()
        assert payload[name] == [f"objects/{hashlib.sha256(contents).hexdigest()}"]
        assert files[payload[name][0]] == contents
    assert files["licenses/loaders/fdl2/README"] == b"original license explanation\n"
    assert files["licenses/loaders/fdl2/gpl-2.0.txt"] == b"original GPL terms\n"
    assert files["licenses/loaders/sml/docs/license.rst"] == b"original monitor license\n"

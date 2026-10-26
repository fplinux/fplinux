# SPDX-License-Identifier: GPL-2.0-only
"""Controlled Windows discovery and native SSH process inputs for host tests."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
from dataclasses import replace
from types import ModuleType, SimpleNamespace
from typing import TYPE_CHECKING, Any
from unittest import mock

from fplinux_cli.build.runtime import runtime_source_files
from fplinux_cli.runtime import bundle as bundle_runtime
from fplinux_cli.runtime import ssh_transport
from fplinux_cli.runtime.windows_host import (
    WindowsBundle,
    WindowsHost,
    WindowsTransport,
)

from tests import ROOT
from tests.bundle_support import file_record
from tests.fixtures.executables import install_python_script
from tests.fixtures.runtime_manifest import runtime_manifest
from tests.small.runtime.adapter_fixtures import adapter_data

if TYPE_CHECKING:
    from pathlib import Path


def admitted_windows(directory: Path) -> WindowsBundle:
    """Exercise complete admission before returning this fixture's native host context."""
    with mock.patch.object(
        bundle_runtime, "os", SimpleNamespace(name="nt", access=os.access, X_OK=os.X_OK)
    ):
        admitted = bundle_runtime.admit_bundle(directory)
    assert admitted.windows is not None
    return admitted.windows


def endpoint_session(directory: Path) -> dict[str, Any]:
    """Describe one exact USB endpoint independently of the production generator."""
    directory.mkdir(parents=True)
    for name in ("id_ed25519", "known_hosts"):
        (directory / name).write_text("fixture private material\n")
    return {
        "target": "test-phone",
        "session_id": "ab" * 32,
        "usb_serial": "0123456789abcdef0123456789abcdef",
        "network": "10.23.45.0/30",
        "host_address": "10.23.45.1",
        "phone_address": "10.23.45.2",
        "host_mac": "02:00:00:00:00:01",
        "device_mac": "02:00:00:00:00:02",
        "vendor_id": 0x0525,
        "product_id": 0xA4A6,
        "directory": str(directory),
        "private_key": str(directory / "id_ed25519"),
        "known_hosts": str(directory / "known_hosts"),
        "status": "ready",
        "prepared_data_sha256": None,
    }


def endpoint_snapshot() -> dict[str, Any]:
    """Represent the read-only PnP, DHCP and route boundary supplied by Windows."""
    return {
        "adapters": [
            {
                "index": 17,
                "mac": "02-00-00-00-00-01",
                "lineage": [
                    "USB\\VID_0525&PID_A4A6&MI_00\\interface-instance",
                    "USB\\VID_0525&PID_A4A6\\0123456789abcdef0123456789abcdef",
                ],
                "compatible": ["USB\\MS_COMP_RNDIS"],
                "provider": "Microsoft",
                "status": "Up",
                "dhcp": "Enabled",
                "addresses": [
                    {
                        "address": "10.23.45.1",
                        "prefix": 30,
                        "state": "Preferred",
                        "origin": "Dhcp",
                    }
                ],
            }
        ],
        "routes": [
            {"index": 17, "prefix": "10.23.45.0/30", "next_hop": "0.0.0.0"},  # noqa: S104
            {"index": 3, "prefix": "0.0.0.0/0", "next_hop": "192.0.2.1"},
        ],
    }


class StubWindowsHost(WindowsHost):
    """Replace unavailable Windows discovery and ACLs; never call native host tools."""

    def __init__(self, snapshot: dict[str, Any]) -> None:
        """Keep the supplied controlled PnP response."""
        self.snapshot = snapshot

    def ps(self, script: str, *arguments: str, timeout: float = 20) -> dict[str, Any]:
        """Return a synthetic snapshot rather than execute PowerShell."""
        del script, arguments, timeout
        return self.snapshot

    def require_private(self, path: Path) -> None:
        """Check fixture existence; this is no evidence about Windows ACL behavior."""
        assert path.exists()

    def secure_new(self, path: Path, *, directory: bool = False) -> None:
        """Leave ACL handling at the explicitly replaced Windows boundary."""
        del directory
        assert path.exists()


def native_process_transport(directory: Path) -> WindowsTransport:
    """Use real child processes with a fixture replacing the remote SSH peer."""
    tools = directory / "tools"
    tools.mkdir()
    for name in ("ssh", "keyscan"):
        destination = tools / name
        shutil.copyfile(ROOT / "tests/fixtures/ssh_transport/windows_peer.py", destination)
        destination.chmod(0o755)
    bundle = WindowsBundle(
        directory,
        {
            "target": "test-phone",
            "usb": {
                "linux_gadget": {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 3}
            },
        },
        {"ssh": tools / "ssh", "keyscan": tools / "keyscan"},
        ModuleType("unused_adapter"),
        ssh_transport,
        "cd" * 32,
        "12" * 32,
    )
    return WindowsTransport(StubWindowsHost(endpoint_snapshot()), bundle)


def saved_connection(transport: WindowsTransport) -> dict[str, Any]:
    """Persist an independently constructed ready session with an exact live USB snapshot."""
    session_id = bytes.fromhex("ab" * 32)
    serial = hashlib.sha256(session_id).hexdigest()[:32]
    directory = transport.bundle.root / "data/session" / serial
    session = endpoint_session(directory)
    session.update(
        {
            "usb_serial": serial,
            "generation": transport.bundle.generation,
            "device_identity": transport.bundle.device_identity,
            "image": str(directory / "image.bin"),
        }
    )
    (directory / "session.json").write_text(json.dumps(session), encoding="utf-8")
    snapshot = endpoint_snapshot()
    snapshot["adapters"][0]["lineage"][-1] = f"USB\\VID_0525&PID_A4A6\\{serial}"
    transport.host = StubWindowsHost(snapshot)
    return session


def session_preparation_bundle(directory: Path) -> WindowsBundle:
    """Use real session/image consumers and a controlled native key-generation process."""
    bundle = admitted_windows(archive_files(directory))
    template = bytes(512)
    image = b"DHTB" + b"p" * 1020 + template + b"unchanged suffix"
    (directory / bundle.runtime["image"]).write_bytes(image)
    runtime = dict(bundle.runtime)
    runtime["personalization"] = {
        "offset": 1024,
        "bytes": 512,
        "template_sha256": hashlib.sha256(template).hexdigest(),
    }
    install_python_script(
        ROOT / "tests/fixtures/runtime-tools/windows_keygen.py", bundle.tools["keygen"]
    )
    return replace(bundle, runtime=runtime)


def archive_files(directory: Path) -> Path:
    """Create a canonical synthetic archive and native file closure for its real consumer."""
    runtime = runtime_manifest()
    runtime["adapter"] = adapter_data()
    runtime["host_tools"] = {
        "loader": "tools/spd_dump.exe",
        "bridge": "tools/libc_server.exe",
    }
    runtime["sha256"].pop("host/loader")
    sources = runtime_source_files(
        "demo",
        {"platform": "ums9117", "linux": {"root": {"kind": "initramfs"}}},
    )
    runtime["device_data"] = None
    runtime["nand"] = None
    runtime["sha256"] = {}
    files = {
        "image/ramboot.bin": b"DHTB synthetic image\n",
        "assets/fdl1.bin": b"synthetic first stage\n",
        "tools/spd_dump.exe": b"synthetic native loader\n",
        "tools/libc_server.exe": b"synthetic native bridge\n",
    }
    for relative, source in sources.items():
        files[relative] = source.read_bytes()
    for relative, payload in files.items():
        destination = directory / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload)
        runtime["sha256"][relative] = hashlib.sha256(payload).hexdigest()
    runtime_path = directory / "runtime-manifest.json"
    runtime_path.write_text(json.dumps(runtime))
    manifest = {
        "rootfs_receipt": None,
        "kernel_artifacts": {},
        "boot_artifacts": None,
        "environment_recipe": None,
        "environment_content": None,
        "apk_signing_key": None,
        "device_identity": "12" * 32,
        "files": {
            "runtime-manifest.json": file_record(runtime_path),
            "image/ramboot.bin": file_record(directory / "image/ramboot.bin"),
        },
        "kbuild_receipt": None,
        "linux_recipe": None,
        "profile": None,
        "build_type": "release",
        "target": "demo",
        "workspace_digest": None,
    }
    canonical = (json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n").encode()
    manifest["generation"] = hashlib.sha256(canonical).hexdigest()
    (directory / "build-manifest.json").write_text(json.dumps(manifest))
    tools = {
        "loader": "tools/spd_dump.exe",
        "bridge": "tools/libc_server.exe",
        "ssh": "tools/openssh/ssh.exe",
        "keygen": "tools/openssh/ssh-keygen.exe",
        "keyscan": "tools/openssh/ssh-keyscan.exe",
        "driver": "tools/driver/wdi-simple.exe",
    }
    hashes = {}
    for source in sorted((ROOT / "common/windows").rglob("*.ps1")):
        relative = source.relative_to(ROOT).as_posix()
        payload = source.read_bytes()
        destination = directory / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload)
        if relative not in runtime["sha256"]:
            hashes[relative] = hashlib.sha256(payload).hexdigest()
    for role, relative in tools.items():
        destination = directory / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        payload = f"synthetic native {role}\n".encode()
        destination.write_bytes(payload)
        if relative not in runtime["sha256"]:
            hashes[relative] = hashlib.sha256(payload).hexdigest()
    (directory / "windows-host.json").write_text(
        json.dumps({"host_tools": tools, "sha256": hashes})
    )
    return directory

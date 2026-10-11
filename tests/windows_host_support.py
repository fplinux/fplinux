# SPDX-License-Identifier: GPL-2.0-only
"""Controlled Windows discovery and native SSH process inputs for host tests."""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import replace
from types import ModuleType
from typing import TYPE_CHECKING, Any

from fplinux_cli.runtime import ssh_transport

from common.windows_host import WindowsBundle, WindowsHost, WindowsTransport, validate_archive
from tests import ROOT
from tests.bundle_support import file_record
from tests.fixtures.executables import install_python_script
from tests.small.runtime.adapter_fixtures import adapter_data
from tests.small.runtime.test_runner_manifest import runtime_manifest

if TYPE_CHECKING:
    from pathlib import Path


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
        {"target": "test-phone"},
        {"ssh": tools / "ssh", "keyscan": tools / "keyscan"},
        ModuleType("unused_adapter"),
        ssh_transport,
        "cd" * 32,
        "12" * 32,
    )
    return WindowsTransport(StubWindowsHost(endpoint_snapshot()), bundle)


def session_preparation_bundle(directory: Path) -> WindowsBundle:
    """Use real session/image consumers and a controlled native key-generation process."""
    bundle = validate_archive(archive_files(directory))
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
        ROOT / "tests/fixtures/processes/windows_keygen.py", bundle.tools["keygen"]
    )
    return replace(bundle, runtime=runtime)


def archive_files(directory: Path) -> Path:
    """Create a canonical synthetic archive and native file closure for its real consumer."""
    runtime = runtime_manifest()
    runtime["adapter"] = adapter_data()
    sources = {
        "runner/run.py": ROOT / "common/run.py",
        "runner/identity.py": ROOT / "scripts/fplinux_cli/manifests/identity.py",
        "runner/platform_adapter.py": ROOT / "platforms/ums9117/host/adapter.py",
        "runner/ssh_transport.py": ROOT / "scripts/fplinux_cli/runtime/ssh_transport.py",
        "runner/loader_events.py": ROOT / "common/loader_events.py",
    }
    files = {
        "image/ramboot.bin": b"DHTB synthetic image\n",
        "assets/fdl1.bin": b"synthetic first stage\n",
        "host/loader": b"canonical host loader\n",
    }
    for relative, source in sources.items():
        files[relative] = source.read_bytes()
    for relative, payload in files.items():
        destination = directory / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload)
        if relative != "runner/run.py":
            runtime["sha256"][relative] = hashlib.sha256(payload).hexdigest()
    runtime_path = directory / "runtime-manifest.json"
    runtime_path.write_text(json.dumps(runtime))
    manifest = {
        "rootfs_receipt": None,
        "boot_artifacts": None,
        "container_image_recipe": None,
        "container_image_content": None,
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
        hashes[relative] = hashlib.sha256(payload).hexdigest()
    for role, relative in tools.items():
        destination = directory / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        payload = f"synthetic native {role}\n".encode()
        destination.write_bytes(payload)
        hashes[relative] = hashlib.sha256(payload).hexdigest()
    license_path = directory / "licenses/windows-driver/License.rtf"
    license_path.parent.mkdir(parents=True)
    license_payload = b"{\\rtf1 Synthetic Microsoft license terms.}"
    license_path.write_bytes(license_payload)
    hashes["licenses/windows-driver/License.rtf"] = hashlib.sha256(license_payload).hexdigest()
    (directory / "windows-host.json").write_text(
        json.dumps({"host_tools": tools, "sha256": hashes})
    )
    return directory

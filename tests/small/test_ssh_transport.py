# SPDX-License-Identifier: GPL-2.0-only
"""Small behavior tests for session-bound USB-NCM SSH transport."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import ssh_transport

from tests.ssh_transport_support import TEST_BUNDLE_IDENTITY, create_ready_session


def ieee_crc32(data: bytes) -> int:
    """Compute the Ethernet CRC-32 independently of the transport implementation."""
    remainder = 0xFFFFFFFF
    for byte in data:
        remainder ^= byte
        for _bit in range(8):
            remainder = (remainder >> 1) ^ (0xEDB88320 if remainder & 1 else 0)
    return remainder ^ 0xFFFFFFFF


class SshTransportSmallTests(unittest.TestCase):
    """Exercise in-process session, bundle and transfer boundaries."""

    def setUp(self) -> None:
        """Create an isolated user runtime root and one synthetic bundle identity."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "runtime"
        self.root.mkdir(mode=0o700)
        self.identity = TEST_BUNDLE_IDENTITY

    def _session(self, *, status: str = "ready") -> dict[str, Any]:
        return create_ready_session(self.root, status=status, identity=self.identity)

    def test_open_shell_rejects_a_noninteractive_process(self) -> None:
        """Do not launch forced-PTY SSH when the host has no input terminal."""
        session = self._session()
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch("fplinux_cli.ssh_transport.os.isatty", return_value=False),
            mock.patch("fplinux_cli.ssh_transport.os.execv") as execute,
            self.assertRaisesRegex(SystemExit, "interactive SSH requires a terminal"),
        ):
            ssh_transport.open_shell(session)

        execute.assert_not_called()

    def test_open_shell_executes_for_an_input_terminal(self) -> None:
        """Keep forced remote PTY allocation for an interactive host terminal."""
        session = self._session()
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch("fplinux_cli.ssh_transport.os.isatty", return_value=True),
            mock.patch.object(
                ssh_transport,
                "_ssh_argv",
                return_value=["/usr/bin/ssh", "fplinux"],
            ),
            mock.patch("fplinux_cli.ssh_transport.os.execv") as execute,
        ):
            ssh_transport.open_shell(session)

        execute.assert_called_once_with(
            "/usr/bin/ssh",
            ["/usr/bin/ssh", "-tt", "fplinux"],
        )

    def test_bundle_identity_rejects_a_runtime_image_outside_its_build_manifest(self) -> None:
        """Refuse reconnect state when the RAM payload no longer matches the generation."""
        bundle = Path(self.temporary.name) / "bundle"
        image = bundle / "image/ramboot.bin"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"DHTB image\n")
        runtime = {
            "target": "phone",
            "profile": None,
            "image": "image/ramboot.bin",
            "sha256": {"image/ramboot.bin": hashlib.sha256(image.read_bytes()).hexdigest()},
        }
        runtime_path = bundle / "runtime-manifest.json"
        runtime_path.write_text(json.dumps(runtime, sort_keys=True) + "\n", encoding="utf-8")

        def record(path: Path) -> dict[str, int | str]:
            return {
                "mode": path.stat().st_mode & 0o777,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "size": path.stat().st_size,
            }

        payload = {
            "rootfs_receipt": {"recipe": "5" * 64, "sha256": "6" * 64},
            "boot_artifacts": {"required": []},
            "container_image_recipe": "7" * 64,
            "container_image_generation": "4" * 64,
            "apk_signing_key": "8" * 64,
            "device_identity": "9" * 64,
            "files": {
                "image/ramboot.bin": record(image),
                "runtime-manifest.json": record(runtime_path),
            },
            "kbuild_receipt": {"recipe": "a" * 64, "sha256": "b" * 64},
            "linux_recipe": "c" * 64,
            "profile": None,
            "target": "phone",
            "workspace_digest": "d" * 64,
        }
        canonical = (
            json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
        ).encode()
        generation = hashlib.sha256(canonical).hexdigest()
        manifest = {**payload, "generation": generation}
        manifest_path = bundle / "build-manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        identity = ssh_transport.bundle_identity(bundle, runtime)
        self.assertEqual(identity, {"bundle_generation": generation})

        image.write_bytes(b"DHTB changed\n")
        with self.assertRaisesRegex(SystemExit, "runtime closure differs"):
            ssh_transport.bundle_identity(bundle, runtime)

    def test_bundle_identity_rejects_a_runtime_from_another_profile(self) -> None:
        """A named profile cannot reuse the default bundle's SSH identity."""
        bundle = Path(self.temporary.name) / "bundle"
        image = bundle / "image/ramboot.bin"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"DHTB image\n")
        runtime = {
            "target": "phone",
            "profile": None,
            "image": "image/ramboot.bin",
            "sha256": {"image/ramboot.bin": hashlib.sha256(image.read_bytes()).hexdigest()},
        }
        runtime_path = bundle / "runtime-manifest.json"
        runtime_path.write_text(json.dumps(runtime, sort_keys=True) + "\n", encoding="utf-8")

        def record(path: Path) -> dict[str, int | str]:
            return {
                "mode": path.stat().st_mode & 0o777,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "size": path.stat().st_size,
            }

        payload = {
            "rootfs_receipt": {"recipe": "5" * 64, "sha256": "6" * 64},
            "boot_artifacts": {"required": []},
            "container_image_recipe": "7" * 64,
            "container_image_generation": "4" * 64,
            "apk_signing_key": "8" * 64,
            "device_identity": "9" * 64,
            "files": {
                "image/ramboot.bin": record(image),
                "runtime-manifest.json": record(runtime_path),
            },
            "kbuild_receipt": {"recipe": "a" * 64, "sha256": "b" * 64},
            "linux_recipe": "c" * 64,
            "profile": None,
            "target": "phone",
            "workspace_digest": "d" * 64,
        }
        encoded = (
            json.dumps(
                payload,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=True,
            )
            + "\n"
        )
        generation = hashlib.sha256(encoded.encode()).hexdigest()
        (bundle / "build-manifest.json").write_text(
            json.dumps({**payload, "generation": generation}, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(SystemExit, "runtime target or profile"):
            ssh_transport.bundle_identity(bundle, {**runtime, "profile": "usb-host-lab"})

    def test_current_session_rejects_another_bundle_generation(self) -> None:
        """A rebuilt bundle cannot silently reuse a phone loaded from older bytes."""
        state = self._session()
        current = self.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(state), encoding="utf-8")
        (current / "phone.json").chmod(0o600)
        changed = {**self.identity, "bundle_generation": "9" * 64}

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            self.assertRaisesRegex(SystemExit, "stale bundle"),
        ):
            ssh_transport.load_current_session("phone", changed)

    def test_current_session_rejects_an_unknown_field(self) -> None:
        """An unrecognized host-state record is not a reconnect session."""
        state = {**self._session(), "unexpected": "value"}
        current = self.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(state), encoding="utf-8")
        (current / "phone.json").chmod(0o600)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            self.assertRaisesRegex(SystemExit, "unexpected fields"),
        ):
            ssh_transport.load_current_session("phone", self.identity)

    def test_usb_session_text_is_exactly_seven_fields_with_nul_padding(self) -> None:
        """Emit the canonical fixed USB-gadget consumer record."""
        config = ssh_transport._usb_config(  # noqa: SLF001
            "0123456789abcdef0123456789abcdef",
            ipaddress.IPv4Network("10.23.45.0/30"),
            "02:00:00:00:00:01",
            "02:00:00:00:00:02",
        )
        expected = (
            "usb_serial=0123456789abcdef0123456789abcdef\n"
            "phone_address=10.23.45.2\n"
            "host_address=10.23.45.1\n"
            "netmask=255.255.255.252\n"
            "broadcast=10.23.45.3\n"
            "device_mac=02:00:00:00:00:02\n"
            "host_mac=02:00:00:00:00:01\n"
        ).encode("ascii")

        self.assertEqual(config, expected.ljust(256, b"\0"))

    def test_session_block_uses_current_fixed_abi_header(self) -> None:
        """Emit the exact RAM-session header consumed by the bundled bootstrap."""
        session_id = bytes(range(32))
        rng_seed = bytes(range(64))
        public_key = b"A" * 68
        usb_config = b"usb_serial=0123456789abcdef0123456789abcdef\n".ljust(256, b"\0")

        block = ssh_transport._session_block(  # noqa: SLF001
            session_id,
            rng_seed,
            public_key,
            usb_config,
        )

        self.assertEqual(len(block), 512)
        self.assertEqual(block[:8], b"FPLSESS\0")
        self.assertEqual(block[8:12], b"\0" * 4)
        self.assertEqual(block[12:16], (512).to_bytes(4, "little"))
        self.assertEqual(block[16:48], session_id)
        self.assertEqual(block[48:112], rng_seed)
        self.assertEqual(block[112:180], public_key)
        self.assertEqual(block[180:436], usb_config)
        self.assertEqual(block[-4:], ieee_crc32(block[:-4]).to_bytes(4, "little"))

    def test_reacquire_retries_mocked_usb_ncm_and_ssh_boundaries(self) -> None:
        """Retry transient failures reported by controlled USB, NCM, and SSH boundaries."""
        state = self._session()
        failures = [
            subprocess.CompletedProcess([], 255, stdout="", stderr="not ready"),
            subprocess.CompletedProcess([], 0, stdout=f"{state['session_id']}\n", stderr=""),
        ]
        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.object(ssh_transport, "_usb_devices", return_value=[Path("/usb/phone")]),
            mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb1"),
            mock.patch.object(ssh_transport, "_network_ready", return_value=True),
            mock.patch.object(ssh_transport, "_retry_pause"),
            mock.patch.object(ssh_transport, "_ssh_argv", return_value=["ssh"]),
            mock.patch("fplinux_cli.ssh_transport.subprocess.run", side_effect=failures),
        ):
            ready = ssh_transport.reacquire_bound_session(state)
            ssh_transport.finish_session(state)

        self.assertEqual(ready["interface"], "usb1")
        self.assertEqual(ready["session_id"], state["session_id"])
        self.assertTrue(Path(state["private_key"]).is_file())

    def test_failed_current_config_publication_removes_the_ready_pointer_and_session(self) -> None:
        """Never leave a direct config pointing at an incomplete ready session."""
        session = self._session()
        write_private_text = ssh_transport._write_private_text  # noqa: SLF001

        def fail_config(path: Path, value: str) -> None:
            if path.name == "phone.ssh-config":
                message = "disk full"
                raise OSError(message)
            write_private_text(path, value)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.object(ssh_transport, "_write_private_text", side_effect=fail_config),
            self.assertRaisesRegex(OSError, "disk full"),
        ):
            ssh_transport._mark_current(session)  # noqa: SLF001

        current = self.root / "current"
        self.assertFalse((current / "phone.json").exists())
        self.assertFalse((current / "phone.ssh-config").exists())
        self.assertEqual(list((self.root / "sessions").iterdir()), [])

    def test_failed_prepare_erases_keys_and_invalidates_prior_current(self) -> None:
        """A failed new RAM load leaves no usable pointer or prepared key directory."""
        prior = self._session()
        current = self.root / "current"
        current.mkdir(mode=0o700)
        (current / "phone.json").write_text(json.dumps(prior), encoding="utf-8")
        image = Path(self.temporary.name) / "ramboot.bin"
        image.write_bytes(b"DHTB" + b"\0" * 1532)
        descriptor = {
            "offset": 1024,
            "bytes": ssh_transport.SESSION_BYTES,
            "template_sha256": hashlib.sha256(b"\0" * 512).hexdigest(),
        }
        keygen_failure = subprocess.CompletedProcess([], 1, stdout="", stderr="keygen failed")

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.object(ssh_transport, "_require_tool", side_effect=lambda name: name),
            mock.patch.object(
                ssh_transport,
                "_choose_network",
                return_value=ipaddress.IPv4Network("10.23.45.0/30"),
            ),
            mock.patch.object(
                ssh_transport,
                "_mac_pair",
                return_value=("02:00:00:00:00:01", "02:00:00:00:00:02"),
            ),
            mock.patch("fplinux_cli.ssh_transport.subprocess.run", return_value=keygen_failure),
            self.assertRaisesRegex(SystemExit, "ssh-keygen failed"),
        ):
            ssh_transport.prepare_session(
                image,
                descriptor,
                "phone",
                {"vendor_id": 0x0525, "product_id": 0xA4A6, "wait_seconds": 1},
                self.identity,
            )

        self.assertFalse((current / "phone.json").exists())
        self.assertEqual(list((self.root / "sessions").iterdir()), [])

    def test_pull_keeps_destination_when_mocked_remote_changes_during_download(self) -> None:
        """Keep the real local destination when the controlled remote boundary changes."""
        destination = Path(self.temporary.name) / "download.bin"
        destination.write_bytes(b"old")
        expected_hash = hashlib.sha256(b"data").hexdigest()
        changed_hash = hashlib.sha256(b"next").hexdigest()

        def download(_session: object, command: str) -> subprocess.CompletedProcess[str]:
            _verb, _remote, local = shlex.split(command)
            Path(local).write_bytes(b"data")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        with (
            mock.patch.object(ssh_transport, "_validate_session", side_effect=lambda value: value),
            mock.patch.object(
                ssh_transport,
                "_remote_metadata",
                side_effect=[(4, expected_hash), (4, changed_hash)],
            ),
            mock.patch.object(ssh_transport, "_sftp", side_effect=download),
            self.assertRaisesRegex(SystemExit, "changed while it was downloaded"),
        ):
            ssh_transport.pull({}, "/root/source.bin", str(destination))

        self.assertEqual(destination.read_bytes(), b"old")
        self.assertEqual(list(destination.parent.glob(f".{destination.name}.*")), [])


if __name__ == "__main__":
    unittest.main()

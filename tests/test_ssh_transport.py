# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for session-bound USB-NCM SSH transport."""

from __future__ import annotations

import base64
import hashlib
import ipaddress
import json
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import ssh_transport


class SshTransportTests(unittest.TestCase):
    """Exercise bundle binding, reconnect and transfer publication boundaries."""

    def setUp(self) -> None:
        """Create an isolated user runtime root and one synthetic bundle identity."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "runtime"
        self.root.mkdir(mode=0o700)
        self.identity = {"bundle_generation": "1" * 64}

    def _session(self, *, status: str = "ready") -> dict[str, Any]:
        session_id = b"session identity is exactly 32!!"
        self.assertEqual(len(session_id), 32)
        usb_serial = hashlib.sha256(session_id).hexdigest()[:32]
        directory = self.root / "sessions" / f"phone.{usb_serial}"
        directory.mkdir(parents=True, mode=0o700)
        owner = {
            "kind": ssh_transport.SESSION_OWNER_KIND,
            "target": "phone",
            "usb_serial": usb_serial,
        }
        (directory / "owner.json").write_text(json.dumps(owner), encoding="utf-8")
        (directory / "owner.json").chmod(0o600)
        private_key = directory / "client_ed25519"
        known_hosts = directory / "known_hosts"
        for path in (private_key, known_hosts):
            path.write_text("private\n", encoding="ascii")
            path.chmod(0o600)
        state: dict[str, Any] = {
            "target": "phone",
            "session_id": session_id.hex(),
            "usb_serial": usb_serial,
            "network": "10.23.45.0/30",
            "host_address": "10.23.45.1",
            "phone_address": "10.23.45.2",
            "host_mac": "02:00:00:00:00:01",
            "device_mac": "02:00:00:00:00:02",
            "private_key": str(private_key),
            "known_hosts": str(known_hosts),
            "image": str(directory / "ramboot.bin"),
            "vendor_id": 0x0525,
            "product_id": 0xA4A6,
            "wait_seconds": 1,
            "status": status,
            "interface": "usb0",
            **self.identity,
        }
        (directory / "session.json").write_text(json.dumps(state), encoding="utf-8")
        (directory / "session.json").chmod(0o600)
        return state

    def test_bundle_identity_rejects_a_runtime_image_outside_its_build_manifest(self) -> None:
        """Refuse reconnect state when the RAM payload no longer matches the generation."""
        bundle = Path(self.temporary.name) / "bundle"
        image = bundle / "image/ramboot.bin"
        image.parent.mkdir(parents=True)
        image.write_bytes(b"DHTB image\n")
        runtime = {
            "target": "phone",
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
            "container_image_recipe": "7" * 64,
            "apk_signing_key": "8" * 64,
            "device_identity": "9" * 64,
            "files": {
                "image/ramboot.bin": record(image),
                "runtime-manifest.json": record(runtime_path),
            },
            "kbuild_receipt": {"recipe": "a" * 64, "sha256": "b" * 64},
            "linux_recipe": "c" * 64,
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
        self.assertEqual(
            block[ssh_transport.SESSION_CRC_OFFSET :],
            zlib.crc32(block[: ssh_transport.SESSION_CRC_OFFSET]).to_bytes(4, "little"),
        )

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

    def test_network_ready_accepts_iproute2_connected_route(self) -> None:
        """Recognize the exact address and connected-route JSON emitted by iproute2."""
        tool_directory = Path(self.temporary.name) / "bin"
        tool_directory.mkdir()
        ip_tool = tool_directory / "ip"
        ip_tool.write_text(
            """#!/bin/sh
case "$*" in
"-4 -j address show dev usb0")
  printf '%s\n' '[{"addr_info":[{"family":"inet","local":"10.23.45.1","prefixlen":30}]}]'
  ;;
"-4 -j route show 10.23.45.0/30")
  printf '%s\n' '[{"dst":"10.23.45.0/30","dev":"usb0"}]'
  ;;
*) exit 2 ;;
esac
""",
            encoding="ascii",
        )
        ip_tool.chmod(0o755)
        session = self._session()

        with mock.patch.dict(os.environ, {"PATH": str(tool_directory)}):
            self.assertTrue(ssh_transport._network_ready("usb0", session))  # noqa: SLF001

    def test_initial_binding_retries_external_tools_and_enforces_key_policy(self) -> None:
        """Run production keyscan and SSH argv against controlled external-tool stubs."""
        state = self._session(status="prepared")
        Path(state["known_hosts"]).unlink()
        tool_directory = Path(self.temporary.name) / "ssh-tools"
        tool_directory.mkdir()
        scan_count = tool_directory / "scan-count"
        ssh_arguments = tool_directory / "ssh-arguments"
        host_blob = b"\0\0\0\x0bssh-ed25519\0\0\0\x20" + bytes(range(32))
        host_key = base64.b64encode(host_blob).decode("ascii")
        keyscan = tool_directory / "ssh-keyscan"
        keyscan.write_text(
            f"""#!/bin/sh
count=0
[ ! -f "{scan_count}" ] || count=$(cat "{scan_count}")
count=$((count + 1))
printf '%s\n' "$count" >"{scan_count}"
[ "$count" -gt 1 ] || exit 1
printf '%s\n' '10.23.45.2 ssh-ed25519 {host_key}'
""",
            encoding="ascii",
        )
        ssh = tool_directory / "ssh"
        ssh.write_text(
            f"""#!/bin/sh
printf '%s\n' "$@" >"{ssh_arguments}"
printf '%s\n' '{state["session_id"]}'
""",
            encoding="ascii",
        )
        keyscan.chmod(0o755)
        ssh.chmod(0o755)

        with (
            mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root),
            mock.patch.object(ssh_transport, "_usb_devices", return_value=[Path("/usb/phone")]),
            mock.patch.object(ssh_transport, "_ncm_interface", return_value="usb0"),
            mock.patch.object(ssh_transport, "_network_ready", return_value=True),
            mock.patch.object(ssh_transport, "_retry_pause"),
            mock.patch.dict(
                os.environ,
                {"PATH": f"{tool_directory}:{os.environ.get('PATH', '')}"},
            ),
            mock.patch("builtins.print"),
        ):
            ready = ssh_transport.wait_for_bound_session(state)

        self.assertEqual(ready["status"], "ready")
        self.assertEqual(scan_count.read_text(encoding="ascii"), "2\n")
        self.assertIn(host_key, Path(state["known_hosts"]).read_text(encoding="ascii"))
        arguments = set(ssh_arguments.read_text(encoding="ascii").splitlines())
        self.assertTrue(
            {
                "StrictHostKeyChecking=yes",
                "PasswordAuthentication=no",
                "KbdInteractiveAuthentication=no",
                "ClearAllForwardings=yes",
                "ProxyCommand=none",
                f"BindAddress={state['host_address']}",
                f"IdentityFile={state['private_key']}",
            }
            <= arguments
        )

    def test_ready_session_publishes_a_private_direct_ssh_config_and_cleanup_removes_it(
        self,
    ) -> None:
        """Publish a usable ``ssh -F`` config only for the ready bound session."""
        session = self._session()
        with mock.patch.object(ssh_transport, "_runtime_root", return_value=self.root):
            ssh_transport._mark_current(session)  # noqa: SLF001

            path = self.root / "current" / "phone.ssh-config"
            metadata = path.lstat()
            self.assertTrue(path.is_file())
            self.assertFalse(path.is_symlink())
            self.assertEqual(metadata.st_mode & 0o777, 0o600)

            directives: dict[str, str] = {}
            hosts: list[str] = []
            for line in path.read_text(encoding="utf-8").splitlines():
                fields = shlex.split(line)
                if fields[0] == "Host":
                    hosts = fields[1:]
                else:
                    self.assertEqual(len(fields), 2)
                    directives[fields[0]] = fields[1]

            self.assertEqual(hosts, ["fplinux"])
            self.assertEqual(
                directives,
                {
                    "HostName": session["phone_address"],
                    "User": "root",
                    "Port": "22",
                    "BatchMode": "yes",
                    "IdentitiesOnly": "yes",
                    "IdentityAgent": "none",
                    "PasswordAuthentication": "no",
                    "KbdInteractiveAuthentication": "no",
                    "PreferredAuthentications": "publickey",
                    "NumberOfPasswordPrompts": "0",
                    "IdentityFile": session["private_key"],
                    "StrictHostKeyChecking": "yes",
                    "UserKnownHostsFile": session["known_hosts"],
                    "GlobalKnownHostsFile": "/dev/null",
                    "HostKeyAlias": f"fplinux-{session['usb_serial']}",
                    "CheckHostIP": "no",
                    "ClearAllForwardings": "yes",
                    "ForwardAgent": "no",
                    "ForwardX11": "no",
                    "Tunnel": "no",
                    "PermitLocalCommand": "no",
                    "ProxyCommand": "none",
                    "ProxyJump": "none",
                    "BindAddress": session["host_address"],
                    "ConnectTimeout": "5",
                    "ConnectionAttempts": "1",
                    "LogLevel": "ERROR",
                },
            )

            ssh = shutil.which("ssh")
            self.assertIsNotNone(ssh)
            parsed = subprocess.run(
                [str(ssh), "-G", "-F", str(path), "fplinux"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(parsed.returncode, 0, parsed.stderr)
            effective = dict(line.split(maxsplit=1) for line in parsed.stdout.splitlines())
            self.assertEqual(effective["hostname"], session["phone_address"])
            self.assertEqual(effective["user"], "root")
            self.assertEqual(effective["identityfile"], session["private_key"])
            self.assertEqual(effective["userknownhostsfile"], session["known_hosts"])
            self.assertEqual(effective["hostkeyalias"], f"fplinux-{session['usb_serial']}")
            self.assertEqual(effective["bindaddress"], session["host_address"])
            self.assertEqual(effective["passwordauthentication"], "no")
            self.assertEqual(effective["identityagent"], "none")
            self.assertEqual(effective["forwardagent"], "no")
            self.assertNotIn("proxycommand", effective)

            ssh_transport._cleanup_target_sessions(self.root, "phone")  # noqa: SLF001

        self.assertFalse(path.exists())
        self.assertFalse((self.root / "current" / "phone.json").exists())

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

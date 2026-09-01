# SPDX-License-Identifier: GPL-2.0-only
"""Runtime-manifest format tests through the shipped RAM runner consumer."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast
from unittest import mock

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[2]


def load_runner() -> ModuleType:
    """Load the standalone runner as a normal Python module."""
    path = ROOT / "common/run.py"
    spec = importlib.util.spec_from_file_location("fplinux_ram_runner", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"unable to load RAM runner: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load_runner()


class PythonRuntimeTests(unittest.TestCase):
    """Keep the standalone runner on its single supported Python series."""

    def test_accepts_python_314(self) -> None:
        """The pinned quality interpreter satisfies the standalone preflight."""
        RUNNER.host_preflight()

    def test_rejects_other_python_series(self) -> None:
        """Older and unqualified newer interpreters fail before phone access."""
        for major, minor in ((3, 13), (3, 15)):
            with (
                self.subTest(version=f"{major}.{minor}"),
                mock.patch.object(
                    RUNNER.sys,
                    "version_info",
                    mock.Mock(major=major, minor=minor),
                ),
                self.assertRaisesRegex(
                    SystemExit,
                    rf"Python 3\.14 is required \(found {major}\.{minor}\)",
                ),
            ):
                RUNNER.host_preflight()


def runtime_manifest() -> dict[str, Any]:
    """Return one complete runtime contract."""
    image = "image/ramboot.bin"
    adapter = "runner/platform_adapter.py"
    identity_helper = "runner/identity.py"
    ssh_helper = "runner/ssh_transport.py"
    fdl1 = "assets/fdl1.bin"
    loader = "host/loader"
    return {
        "target": "demo",
        "profile": None,
        "identity": {
            "target": {
                "brand": "Demo",
                "product": "Phone",
                "hardware_codes": ["D-1", "D-2"],
                "compatible": "demo,phone",
                "display_name": "Demo Phone (D-1, D-2)",
            },
            "platform": {
                "name": "ums9117",
                "vendor": "Unisoc",
                "soc": "UMS9117",
                "aliases": ["T117"],
                "compatible": "sprd,ums9117",
                "display_name": "Unisoc UMS9117",
            },
        },
        "transport": "usb-ncm",
        "image": image,
        "personalization": {
            "offset": 1024,
            "bytes": 512,
            "template_sha256": "e" * 64,
        },
        "addresses": {"fdl1": 0x6200, "payload": 0x80100000},
        "usb": {
            "bootrom": {"vendor_id": 0x1782, "product_id": 0x4D00, "wait_seconds": 30},
            "linux_gadget": {
                "vendor_id": 0x0525,
                "product_id": 0xA4A7,
                "wait_seconds": 30,
                "keyboard_interface": 1,
            },
        },
        "assets": {"fdl1": fdl1},
        "adapter": {"kind": "demo"},
        "host_tools": {"loader": loader},
        "sha256": {
            image: "a" * 64,
            adapter: "b" * 64,
            identity_helper: "1" * 64,
            ssh_helper: "c" * 64,
            fdl1: "d" * 64,
            loader: "f" * 64,
        },
    }


class RuntimeManifestTests(unittest.TestCase):
    """The bundled runner accepts only its exact manifest contract."""

    def setUp(self) -> None:
        """Create one isolated runtime-manifest path."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "runtime-manifest.json"

    def load(self, manifest: dict[str, Any]) -> dict[str, Any]:
        """Load one manifest through the shipped consumer."""
        self.path.write_text(json.dumps(manifest), encoding="utf-8")
        runtime = cast("dict[str, Any]", RUNNER.load_runtime_manifest(self.path))
        return cast("dict[str, Any]", RUNNER.validate_runtime_identity(runtime))

    def test_accepts_the_exact_runtime_contract(self) -> None:
        """Accept a complete manifest with explicit USB interfaces."""
        loaded = self.load(runtime_manifest())

        self.assertEqual(loaded["personalization"]["bytes"], 512)
        self.assertEqual(loaded["usb"]["linux_gadget"]["keyboard_interface"], 1)
        self.assertEqual(
            loaded["identity"]["target"]["display_name"],
            "Demo Phone (D-1, D-2)",
        )

    def test_manifest_shape_does_not_execute_identity_code_before_hash_verification(self) -> None:
        """Keep bundled Python execution behind the runner's declared-file hash loop."""
        self.path.write_text(json.dumps(runtime_manifest()), encoding="utf-8")
        with mock.patch.object(
            RUNNER,
            "identity_module",
            side_effect=AssertionError("identity helper executed too early"),
        ):
            loaded = RUNNER.load_runtime_manifest(self.path)

        self.assertIsInstance(loaded["identity"], dict)

    def test_accepts_an_explicit_no_transport_handoff_contract(self) -> None:
        """A host-only bundle declares that it will not create USB-NCM transport."""
        manifest = runtime_manifest()
        manifest["transport"] = "none"

        loaded = self.load(manifest)

        self.assertEqual(loaded["transport"], "none")

    def test_accepts_identity_without_unverified_codes_or_aliases(self) -> None:
        """An empty token array remains an explicit statement that no code is known."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["hardware_codes"] = []
        manifest["identity"]["target"]["display_name"] = "Demo Phone"
        manifest["identity"]["platform"]["aliases"] = []

        loaded = self.load(manifest)

        self.assertEqual(loaded["identity"]["target"]["hardware_codes"], [])
        self.assertEqual(loaded["identity"]["platform"]["aliases"], [])

    def test_rejects_one_compatible_for_both_target_and_platform(self) -> None:
        """Require an exact machine identity followed by a distinct SoC fallback."""
        manifest = runtime_manifest()
        manifest["identity"]["platform"]["compatible"] = "demo,phone"

        with self.assertRaisesRegex(SystemExit, "compatibles must differ"):
            self.load(manifest)

    def test_rejects_an_unknown_host_transport(self) -> None:
        """Transport selection remains a closed runtime contract."""
        manifest = runtime_manifest()
        manifest["transport"] = "serial"

        with self.assertRaisesRegex(SystemExit, "runtime transport must be one of"):
            self.load(manifest)

    def test_rejects_a_pre_profile_runtime_manifest(self) -> None:
        """A runtime without an explicit profile and transport is not reinterpreted."""
        manifest = runtime_manifest()
        del manifest["profile"]
        del manifest["transport"]

        with self.assertRaisesRegex(SystemExit, "runtime manifest must contain exactly"):
            self.load(manifest)

    def test_rejects_an_unknown_runtime_field(self) -> None:
        """Reject fields outside the exact runtime contract."""
        manifest = runtime_manifest()
        manifest["unexpected"] = "value"

        with self.assertRaisesRegex(SystemExit, "runtime manifest must contain exactly"):
            self.load(manifest)

    def test_rejects_the_legacy_root_identity_fields(self) -> None:
        """Do not reinterpret the previous display_name and platform schema."""
        manifest = runtime_manifest()
        del manifest["identity"]
        manifest["display_name"] = "Demo Phone"
        manifest["platform"] = "ums9117"

        with self.assertRaisesRegex(SystemExit, "runtime manifest must contain exactly"):
            self.load(manifest)

    def test_rejects_noncanonical_identity_text(self) -> None:
        """Identity text is stable printable ASCII without padding or doubled spaces."""
        invalid_values = (" Demo", "Demo  Devices", "Démo", "Demo\nDevices")
        for brand in invalid_values:
            manifest = runtime_manifest()
            manifest["identity"]["target"]["brand"] = brand
            with (
                self.subTest(brand=brand),
                self.assertRaisesRegex(SystemExit, "canonical printable ASCII text"),
            ):
                self.load(manifest)

    def test_rejects_identity_that_does_not_derive_its_display_name(self) -> None:
        """Display names cannot become a second independently editable identity."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["display_name"] = "Independent label"

        with self.assertRaisesRegex(SystemExit, "display_name must be derived"):
            self.load(manifest)

        manifest = runtime_manifest()
        manifest["identity"]["platform"]["display_name"] = "Independent platform"

        with self.assertRaisesRegex(SystemExit, "display_name must be derived"):
            self.load(manifest)

    def test_rejects_invalid_or_duplicate_hardware_tokens(self) -> None:
        """Hardware codes and aliases remain ordered arrays of unique tokens."""
        invalid_values: tuple[object, ...] = (
            "D-1",
            ["lowercase"],
            ["D-1", "D-1"],
        )
        for hardware_codes in invalid_values:
            manifest = runtime_manifest()
            manifest["identity"]["target"]["hardware_codes"] = hardware_codes
            with self.subTest(hardware_codes=hardware_codes), self.assertRaises(SystemExit):
                self.load(manifest)

    def test_rejects_alias_equal_to_the_platform_soc(self) -> None:
        """An alias must add identity instead of repeating the canonical SoC name."""
        manifest = runtime_manifest()
        manifest["identity"]["platform"]["aliases"] = ["UMS9117"]

        with self.assertRaisesRegex(SystemExit, "aliases must not repeat the SoC name"):
            self.load(manifest)

    def test_rejects_noncanonical_compatible(self) -> None:
        """Runtime identity uses one lowercase vendor,device compatible."""
        manifest = runtime_manifest()
        manifest["identity"]["target"]["compatible"] = "Nokia,TA-1618"

        with self.assertRaisesRegex(SystemExit, "lowercase vendor,device compatible"):
            self.load(manifest)

    def test_rejects_runtime_without_declared_keyboard_interface(self) -> None:
        """Require every current Linux USB interface explicitly."""
        manifest = runtime_manifest()
        del manifest["usb"]["linux_gadget"]["keyboard_interface"]

        with self.assertRaisesRegex(SystemExit, "linux_gadget USB must contain exactly"):
            self.load(manifest)

    def test_rejects_a_runtime_without_a_mandatory_runner_helper_hash(self) -> None:
        """Bind both the SSH and identity consumers into the runtime closure."""
        for helper in ("runner/ssh_transport.py", "runner/identity.py"):
            manifest = runtime_manifest()
            del manifest["sha256"][helper]

            with (
                self.subTest(helper=helper),
                self.assertRaisesRegex(SystemExit, "runtime hashes must contain exactly"),
            ):
                self.load(manifest)


class NoTransportRunnerTests(unittest.TestCase):
    """Exercise the shipped runner's no-transport handoff boundary."""

    def setUp(self) -> None:
        """Build one regular hashed bundle that has no USB-NCM transport."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.bundle = Path(self.temporary.name) / "bundle"
        self.runner = self.bundle / "runner/run.py"
        manifest = runtime_manifest()
        manifest["transport"] = "none"
        payloads = {
            "image/ramboot.bin": b"DHTB RAM image\n",
            "runner/identity.py": (ROOT / "scripts/fplinux_cli/identity.py").read_bytes(),
            "runner/platform_adapter.py": b"adapter\n",
            "runner/ssh_transport.py": b"unused but hashed helper\n",
            "assets/fdl1.bin": b"fdl1\n",
            "host/loader": b"loader\n",
        }
        for relative, data in payloads.items():
            path = self.bundle / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o755 if relative == "host/loader" else 0o644)
            manifest["sha256"][relative] = hashlib.sha256(data).hexdigest()
        self.runner.parent.mkdir(parents=True, exist_ok=True)
        self.runner.write_text("#!/usr/bin/env python3\n", encoding="utf-8")
        self.runner.chmod(0o755)
        (self.bundle / "runtime-manifest.json").write_text(
            json.dumps(manifest),
            encoding="utf-8",
        )
        runtime_path = self.bundle / "runtime-manifest.json"

        def record(path: Path) -> dict[str, int | str]:
            return {
                "mode": path.stat().st_mode & 0o777,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "size": path.stat().st_size,
            }

        payload = {
            "rootfs_receipt": {"recipe": "a" * 64, "sha256": "b" * 64},
            "boot_artifacts": {"required": []},
            "container_image_recipe": "c" * 64,
            "container_image_generation": "9" * 64,
            "apk_signing_key": "d" * 64,
            "device_identity": "e" * 64,
            "files": {
                "image/ramboot.bin": record(self.bundle / "image/ramboot.bin"),
                "runtime-manifest.json": record(runtime_path),
            },
            "kbuild_receipt": {"recipe": "f" * 64, "sha256": "0" * 64},
            "linux_recipe": "1" * 64,
            "profile": None,
            "target": "demo",
            "workspace_digest": "2" * 64,
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
        self.generation = hashlib.sha256(encoded.encode()).hexdigest()
        (self.bundle / "build-manifest.json").write_text(
            json.dumps({**payload, "generation": self.generation}, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    def test_none_transport_personalizes_without_waiting_for_usb_ncm(self) -> None:
        """A host-only run still creates its required RAM session image before handoff."""
        adapter = mock.Mock()
        session = {"image": str(self.bundle / "image/ramboot.bin")}
        ssh = mock.Mock()
        ssh.prepare_session.return_value = session
        with (
            mock.patch.object(RUNNER, "__file__", str(self.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(RUNNER, "load_adapter", return_value=adapter),
            mock.patch.object(RUNNER, "load_module", return_value=ssh),
            mock.patch.object(sys, "argv", [str(self.runner)]),
        ):
            RUNNER.main()

        runtime = adapter.run.call_args.args[1]
        self.assertEqual(runtime["transport"], "none")
        self.assertIs(adapter.run.call_args.args[2], session)
        ssh.prepare_session.assert_called_once()
        ssh.wait_for_bound_session.assert_not_called()
        ssh.open_shell.assert_not_called()
        ssh.finish_session.assert_called_once_with(session)

    def test_standalone_reconnect_checks_the_running_device_identity(self) -> None:
        """Reconnect through the bundle manifest's kernel identity, not its generation."""
        runtime_path = self.bundle / "runtime-manifest.json"
        manifest = json.loads(runtime_path.read_text(encoding="utf-8"))
        manifest["transport"] = "usb-ncm"
        runtime_path.write_text(json.dumps(manifest), encoding="utf-8")
        session: dict[str, str] = {}
        ready = {**session, "interface": "usb0"}
        result = mock.Mock(returncode=0)
        ssh = mock.Mock()
        ssh.build_manifest_device_identity.return_value = "e" * 64
        ssh.load_current_session.return_value = session
        ssh.reacquire_bound_session.return_value = ready
        ssh.run_remote.return_value = result

        with (
            mock.patch.object(RUNNER, "__file__", str(self.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(RUNNER, "load_module", return_value=ssh),
            mock.patch.object(
                sys,
                "argv",
                [str(self.runner), "--reconnect", "--exec", "true"],
            ),
        ):
            RUNNER.main()

        ssh.bundle_identity.assert_called_once_with(self.bundle, mock.ANY)
        ssh.build_manifest_device_identity.assert_called_once_with(self.bundle)
        ssh.load_current_session.assert_called_once_with("demo")
        ssh.reacquire_bound_session.assert_called_once_with(session)
        ssh.require_device_identity.assert_called_once_with(ready, "e" * 64)
        ssh.run_remote.assert_called_once_with(ready, "true")

    def test_standalone_reconnect_rejects_another_device_before_remote_action(self) -> None:
        """A mismatched kernel identity prevents the requested standalone command."""
        runtime_path = self.bundle / "runtime-manifest.json"
        manifest = json.loads(runtime_path.read_text(encoding="utf-8"))
        manifest["transport"] = "usb-ncm"
        runtime_path.write_text(json.dumps(manifest), encoding="utf-8")
        session: dict[str, str] = {}
        ready = {**session, "interface": "usb0"}
        ssh = mock.Mock()
        ssh.build_manifest_device_identity.return_value = "e" * 64
        ssh.load_current_session.return_value = session
        ssh.reacquire_bound_session.return_value = ready
        ssh.require_device_identity.side_effect = SystemExit(
            "SSH transport failed: current SSH session exposes a different kernel identity"
        )

        with (
            mock.patch.object(RUNNER, "__file__", str(self.runner)),
            mock.patch.object(RUNNER, "_identity_module", None),
            mock.patch.object(RUNNER, "host_preflight"),
            mock.patch.object(RUNNER, "load_module", return_value=ssh),
            mock.patch.object(
                sys,
                "argv",
                [str(self.runner), "--reconnect", "--exec", "touch /tmp/should-not-run"],
            ),
            self.assertRaisesRegex(SystemExit, "different kernel identity"),
        ):
            RUNNER.main()

        ssh.require_device_identity.assert_called_once_with(ready, "e" * 64)
        ssh.run_remote.assert_not_called()


if __name__ == "__main__":
    unittest.main()

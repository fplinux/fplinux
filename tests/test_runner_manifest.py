# SPDX-License-Identifier: GPL-2.0-only
"""Runtime-manifest format tests through the shipped RAM runner consumer."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any, cast

if TYPE_CHECKING:
    from types import ModuleType

ROOT = Path(__file__).resolve().parents[1]


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


def runtime_manifest() -> dict[str, Any]:
    """Return one complete runtime contract."""
    image = "image/ramboot.bin"
    adapter = "runner/platform_adapter.py"
    ssh_helper = "runner/ssh_transport.py"
    fdl1 = "assets/fdl1.bin"
    loader = "host/loader"
    return {
        "target": "demo",
        "display_name": "Demo",
        "platform": "ums9117",
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
        return cast("dict[str, Any]", RUNNER.load_runtime_manifest(self.path))

    def test_accepts_the_exact_runtime_contract(self) -> None:
        """Accept a complete manifest with explicit USB interfaces."""
        loaded = self.load(runtime_manifest())

        self.assertEqual(loaded["personalization"]["bytes"], 512)
        self.assertEqual(loaded["usb"]["linux_gadget"]["keyboard_interface"], 1)

    def test_rejects_an_unknown_runtime_field(self) -> None:
        """Reject fields outside the exact runtime contract."""
        manifest = runtime_manifest()
        manifest["unexpected"] = "value"

        with self.assertRaisesRegex(SystemExit, "runtime manifest must contain exactly"):
            self.load(manifest)

    def test_rejects_runtime_without_declared_keyboard_interface(self) -> None:
        """Require every current Linux USB interface explicitly."""
        manifest = runtime_manifest()
        del manifest["usb"]["linux_gadget"]["keyboard_interface"]

        with self.assertRaisesRegex(SystemExit, "linux_gadget USB must contain exactly"):
            self.load(manifest)

    def test_rejects_a_runtime_without_the_mandatory_ssh_helper_hash(self) -> None:
        """Do not start an unbound RAM image without its reconnect consumer."""
        manifest = runtime_manifest()
        del manifest["sha256"]["runner/ssh_transport.py"]

        with self.assertRaisesRegex(SystemExit, "runtime hashes must contain exactly"):
            self.load(manifest)


if __name__ == "__main__":
    unittest.main()

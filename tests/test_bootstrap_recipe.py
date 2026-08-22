# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for the device-visible bootstrap recipe closure."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import builder


class BootstrapRecipeTests(unittest.TestCase):
    """Hash only inputs copied or selected by ``build_bootstrap``."""

    def setUp(self) -> None:
        """Create a minimal target/bootstrap projection closure."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self._write("targets/demo/bootstrap/Makefile", b"all:\n\ttrue\n")
        self._write("targets/demo/bootstrap/main.c", b"int entry(void) { return 1; }\n")
        self._write("bootstrap/fplinux-boot-screen/screen.c", b"int screen;\n")
        self.target_config = {
            "bootstrap": {
                "source": "bootstrap",
                "image": "ramboot.bin",
                "map": "obj/ramboot.map",
                "kernel_destination": "zImage",
                "dtb_destination": "target.dtb",
                "load_address": 0x80100000,
                "payload_limit": 0x82000000,
                "toolchain": "arm-none-eabi",
                "lto": 0,
            }
        }
        self.platform = {
            "bootstrap": {
                "vendor_source_lock": "vendor",
                "vendor_cache_name": "vendor.tar.gz",
                "archive_prefix": "vendor-{commit}/",
                "source_destination": "bootstrap",
                "vendor_destination": "vendor",
                "output_destination": "out",
                "pack_reloc": "pack_reloc",
                "safety_target": "fplinux-safety-check",
                "build_targets": ["clean", "all", "map"],
                "files": ["pack_reloc/Makefile"],
                "shared_copies": [
                    {
                        "source": "bootstrap/fplinux-boot-screen",
                        "destination": "fplinux-boot-screen",
                    }
                ],
            }
        }
        self.sources = {
            "vendor": {
                "repository": "https://example.invalid/vendor",
                "commit": "abc123",
                "archive_url": "https://example.invalid/vendor.tar.gz",
                "archive_sha256": "a" * 64,
                "license": "Unlicense",
            }
        }

    def _write(self, relative: str, contents: bytes) -> Path:
        """Write one test repository file."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def _digest(self) -> str:
        """Compute the bootstrap recipe against the temporary repository."""
        with mock.patch.object(builder, "ROOT", self.root):
            return builder.bootstrap_recipe_digest(
                self.sources,
                "demo",
                self.target_config,
                self.platform,
            )

    def test_bootstrap_source_shared_vendor_and_config_are_causal(self) -> None:
        """Every declared bootstrap input changes the exact recipe."""
        baseline = self._digest()

        self._write("targets/demo/bootstrap/main.c", b"int entry(void) { return 2; }\n")
        self.assertNotEqual(baseline, self._digest())
        self._write("targets/demo/bootstrap/main.c", b"int entry(void) { return 1; }\n")

        self._write("bootstrap/fplinux-boot-screen/screen.c", b"int changed_screen;\n")
        self.assertNotEqual(baseline, self._digest())
        self._write("bootstrap/fplinux-boot-screen/screen.c", b"int screen;\n")

        self.sources["vendor"]["archive_sha256"] = "b" * 64
        self.assertNotEqual(baseline, self._digest())
        self.sources["vendor"]["archive_sha256"] = "a" * 64

        self.sources["vendor"]["commit"] = "def456"
        self.assertNotEqual(baseline, self._digest())
        self.sources["vendor"]["commit"] = "abc123"

        self.target_config["bootstrap"]["toolchain"] = "arm-none-eabi-custom"
        self.assertNotEqual(baseline, self._digest())
        self.target_config["bootstrap"]["toolchain"] = "arm-none-eabi"

        self.platform["bootstrap"]["build_targets"] = ["clean", "all"]
        self.assertNotEqual(baseline, self._digest())

    def test_host_and_docs_are_outside_the_bootstrap_closure(self) -> None:
        """Unselected host and documentation files must not relabel the phone image."""
        baseline = self._digest()
        self._write("docs/BUILDING.md", b"unrelated documentation\n")
        self._write("platforms/demo/host/adapter.py", b"unrelated host adapter\n")

        self.sources["vendor"]["repository"] = "https://example.invalid/other"
        self.sources["vendor"]["archive_url"] = "https://example.invalid/other.tar.gz"
        self.sources["vendor"]["license"] = "Other"

        self.assertEqual(baseline, self._digest())


if __name__ == "__main__":
    unittest.main()

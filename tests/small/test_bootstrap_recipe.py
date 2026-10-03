# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for the device-visible bootstrap recipe closure."""

from __future__ import annotations

import io
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import common
from fplinux_cli.build import bootstrap as bootstrap_build
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build


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
        self._write("bootstrap/fplinux-boot-screen/linux/font.h", b"font declarations\n")
        self._write("patches/vendor.patch", b"vendor patch\n")
        self._write("scripts/fplinux_cli/build_env.py", b"build environment\n")
        self._write("scripts/fplinux_cli/build/bootstrap.py", b"builder implementation\n")
        self.target_config: dict[str, Any] = {
            "identity": {
                "brand": "Demo",
                "product": "Phone",
                "hardware_codes": [],
                "compatible": "demo,phone",
                "display_name": "Demo Phone",
            },
            "bootstrap": {
                "kind": "linux",
                "source": "bootstrap",
                "image": "ramboot.bin",
                "map": "obj/ramboot.map",
                "kernel_destination": "zImage",
                "dtb_destination": "target.dtb",
                "record_prefix": "DEMO",
                "load_address": 0x80100000,
                "payload_limit": 0x82000000,
            },
        }
        self.platform: dict[str, Any] = {
            "linux": {"source_lock": "linux"},
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
                "patches": ["patches/vendor.patch"],
                "files": ["pack_reloc/Makefile"],
                "lcd_config_destination": "runtime/lcd_config.h",
                "kernel_destination": "zImage",
                "load_address": 0x80100000,
                "payload_limit": 0x82000000,
                "layout": {
                    "ram_base": 0x80000000,
                    "ram_size": 0x04000000,
                    "timer_hz": 1000,
                    "kernel_load": 0x82000000,
                    "kernel_entry": 0x82000000,
                    "kernel_size": 0x01200000,
                    "fdt_load": 0x83E00000,
                    "fdt_size": 0x00010000,
                    "framebuffer": 0x83F00000,
                    "framebuffer_size": 0x00100000,
                },
                "toolchain": "arm-none-eabi",
                "lto": 0,
                "shared_copies": [
                    {
                        "source": "bootstrap/fplinux-boot-screen",
                        "destination": "fplinux-boot-screen",
                    }
                ],
                "linux_copies": [
                    {
                        "source": "lib/fonts/font_sample.c",
                        "destination": "fplinux-boot-screen/font_sample.c",
                    }
                ],
            },
        }
        self.sources: dict[str, dict[str, str]] = {
            "vendor": {
                "repository": "https://example.invalid/vendor",
                "commit": "abc123",
                "archive_url": "https://example.invalid/vendor.tar.gz",
                "archive_sha256": "a" * 64,
                "license": "Unlicense",
            },
            "linux": {
                "version": "1.2.3",
                "url": "https://example.invalid/linux.tar.xz",
                "sha256": "c" * 64,
            },
        }

    def _write(self, relative: str, contents: bytes) -> Path:
        """Write one test repository file."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def _digest(self) -> str:
        """Compute the bootstrap recipe against the temporary repository."""
        with mock.patch.object(common, "ROOT", self.root):
            return bootstrap_build.bootstrap_recipe_digest(
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

        self._write("patches/vendor.patch", b"changed vendor patch\n")
        self.assertNotEqual(baseline, self._digest())
        self._write("patches/vendor.patch", b"vendor patch\n")

        self.sources["vendor"]["archive_sha256"] = "b" * 64
        self.assertNotEqual(baseline, self._digest())
        self.sources["vendor"]["archive_sha256"] = "a" * 64

        self.sources["vendor"]["commit"] = "def456"
        self.assertNotEqual(baseline, self._digest())
        self.sources["vendor"]["commit"] = "abc123"

        self.platform["bootstrap"]["toolchain"] = "arm-none-eabi-custom"
        self.assertNotEqual(baseline, self._digest())
        self.platform["bootstrap"]["toolchain"] = "arm-none-eabi"

        self.platform["bootstrap"]["build_targets"] = ["clean", "all"]
        self.assertNotEqual(baseline, self._digest())

        self.platform["bootstrap"]["build_targets"] = ["clean", "all", "map"]
        self._write("scripts/fplinux_cli/build_env.py", b"changed environment\n")
        self.assertNotEqual(baseline, self._digest())
        self._write("scripts/fplinux_cli/build_env.py", b"build environment\n")

        self.target_config["identity"]["display_name"] = "Demo Changed Phone"
        self.assertNotEqual(baseline, self._digest())
        self.target_config["identity"]["display_name"] = "Demo Phone"

        self.target_config["bootstrap"]["record_prefix"] = "CHANGED"
        self.assertNotEqual(baseline, self._digest())

    def test_host_and_docs_are_outside_the_bootstrap_closure(self) -> None:
        """Unselected host and documentation files must not relabel the phone image."""
        baseline = self._digest()
        documentation = self._write("docs/BUILDING.md", b"unrelated documentation\n")
        self.assertEqual(baseline, self._digest())
        documentation.unlink()

        host_adapter = self._write(
            "platforms/demo/host/adapter.py",
            b"unrelated host adapter\n",
        )
        self.assertEqual(baseline, self._digest())
        host_adapter.unlink()

        self.sources["vendor"]["repository"] = "https://example.invalid/other"
        self.assertEqual(baseline, self._digest())
        self.sources["vendor"]["repository"] = "https://example.invalid/vendor"

        self.sources["vendor"]["archive_url"] = "https://example.invalid/other.tar.gz"
        self.assertEqual(baseline, self._digest())
        self.sources["vendor"]["archive_url"] = "https://example.invalid/vendor.tar.gz"

        self.sources["vendor"]["license"] = "Other"
        self.assertEqual(baseline, self._digest())

    def test_sd_recipe_tracks_the_shared_target_panel(self) -> None:
        """The RAM-owned panel also invalidates its SD consumer without foreign panel inputs."""
        self._write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        panel = self._write("targets/demo/bootstrap/lcd_config.h", b"selected panel\n")
        self.target_config["bootstrap"].update(
            source="bootstrap-microsd", lcd_config="bootstrap/lcd_config.h"
        )
        baseline = self._digest()

        self._write("targets/another/bootstrap/lcd_config.h", b"foreign panel\n")
        self.assertEqual(baseline, self._digest())
        panel.write_bytes(b"changed selected panel\n")
        self.assertNotEqual(baseline, self._digest())

    def test_linux_font_source_and_freestanding_header_are_causal(self) -> None:
        """A pinned font or its compile interface invalidates either bootstrap recipe."""
        self._write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        for source in ("bootstrap", "bootstrap-microsd"):
            with self.subTest(source=source):
                self.target_config["bootstrap"]["source"] = source
                baseline = self._digest()
                self.sources["linux"]["url"] = "https://example.invalid/mirror.tar.xz"
                self._write("unrelated-font.c", b"int unrelated_font;\n")
                self.assertEqual(baseline, self._digest())
                self.sources["linux"]["sha256"] = "d" * 64
                self.assertNotEqual(baseline, self._digest())
                self.sources["linux"]["sha256"] = "c" * 64
                header = self._write(
                    "bootstrap/fplinux-boot-screen/linux/font.h", b"changed declarations\n"
                )
                self.assertNotEqual(baseline, self._digest())
                header.write_bytes(b"font declarations\n")

    def test_projection_selects_one_panel_or_the_headless_table(self) -> None:
        """The compiler receives the target bytes for either source tree, or the full fallback."""
        archive = self.root / "cache/downloads/vendor.tar.gz"
        archive.parent.mkdir(parents=True)
        with tarfile.open(archive, "w:gz") as output:
            for path, contents in (
                ("pack_reloc/Makefile", b"all:\n\ttrue\n"),
                ("runtime/lcd_config.h", b"complete porting table\n"),
            ):
                member = tarfile.TarInfo(f"vendor-abc123/{path}")
                member.size = len(contents)
                output.addfile(member, io.BytesIO(contents))
        self.sources["vendor"]["archive_sha256"] = common.sha256_file(archive)
        linux_archive = self.root / "cache/downloads/linux/linux-1.2.3.tar.xz"
        linux_archive.parent.mkdir(parents=True)
        with tarfile.open(linux_archive, "w:xz") as output:
            contents = b"int unchanged_upstream_font;\n"
            member = tarfile.TarInfo("linux-1.2.3/lib/fonts/font_sample.c")
            member.size = len(contents)
            output.addfile(member, io.BytesIO(contents))
        self.sources["linux"]["sha256"] = common.sha256_file(linux_archive)
        self.platform["bootstrap"]["patches"] = []
        self._write("targets/demo/bootstrap/lcd_config.h", b"single selected panel\n")
        self._write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        zimage = self._write("kernel/zImage", b"kernel payload\n")
        dtb = self._write("kernel/target.dtb", b"device tree\n")
        work = self.root / "work"
        for source, selected, expected in (
            ("bootstrap", "bootstrap/lcd_config.h", b"single selected panel\n"),
            ("bootstrap-microsd", "bootstrap/lcd_config.h", b"single selected panel\n"),
            ("bootstrap", None, b"complete porting table\n"),
        ):
            with self.subTest(source=source, selected=selected):
                self.target_config["bootstrap"].update(source=source, lcd_config=selected)
                with (
                    mock.patch.object(common, "ROOT", self.root),
                    mock.patch.object(inputs_build, "CACHE", self.root / "cache"),
                    # Compilation is external to this source-projection check.
                    mock.patch.object(
                        process_build,
                        "run",
                        side_effect=RuntimeError("stop before compilation"),
                    ),
                    self.assertRaisesRegex(RuntimeError, "stop before compilation"),
                ):
                    bootstrap_build.build_bootstrap(
                        self.sources,
                        "demo",
                        self.target_config,
                        self.platform,
                        work=work,
                        zimage=zimage,
                        dtb=dtb,
                    )
                panel = work / "bootstrap/vendor/runtime/lcd_config.h"
                self.assertEqual(panel.read_bytes(), expected)
                font = work / "bootstrap/bootstrap/fplinux-boot-screen/font_sample.c"
                self.assertEqual(font.read_bytes(), b"int unchanged_upstream_font;\n")


if __name__ == "__main__":
    unittest.main()

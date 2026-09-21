# SPDX-License-Identifier: GPL-2.0-only
"""Cache-lifecycle tests with a synthetic make-compatible U-Boot source."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import common, uboot_tools
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build import storage as storage_build

ROOT = Path(__file__).resolve().parents[2]
UBOOT_FIXTURES = ROOT / "tests/fixtures/uboot_tools"


class UbootToolsTests(unittest.TestCase):
    """Exercise the producer without claiming a real U-Boot build."""

    def setUp(self) -> None:
        """Create the make-compatible external source boundary used by every case."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.required_config = (UBOOT_FIXTURES / "required.config").read_text(encoding="utf-8")
        source = self.root / "source/u-boot-2026.07"
        self.prepare_synthetic_source(source)
        self.defconfig = self.root / "ta1618_defconfig"
        self.defconfig.write_text(self.required_config, encoding="utf-8")
        self.build_log = self.root / "build.log"
        self.projection = self.root / "build-log.sh"
        shutil.copyfile(UBOOT_FIXTURES / "build-log.sh", self.projection)
        self.record_builds_in(self.build_log)
        self.archive = self.archive_synthetic_source(source)
        self.config = self.full_uboot_config(self.archive)
        self.work = self.root / "work"
        self.layout = self.full_uboot_layout()

    def record_builds_in(self, build_log: Path) -> None:
        """Expose the per-test build log to the projected fixture program."""
        environment = mock.patch.dict(
            os.environ,
            {"FPLINUX_TEST_BUILD_LOG": str(build_log)},
        )
        environment.start()
        self.addCleanup(environment.stop)

    def archive_synthetic_source(self, source: Path) -> Path:
        """Create the compressed source input with the standard tar implementation."""
        archive = self.root / "source.tar.bz2"
        with tarfile.open(archive, "w:bz2") as output:
            output.add(source, arcname="u-boot-2026.07")
        return archive

    @staticmethod
    def full_uboot_config(archive: Path) -> dict[str, Any]:
        """Bind the synthetic archive to the full U-Boot source contract."""
        return {
            "kind": "full",
            "source": "u-boot.lock.toml",
            "archive_prefix": "u-boot-2026.07",
            "lock": {
                "version": "2026.07",
                "repository": "https://source.denx.de/u-boot/u-boot.git",
                "tag": "v2026.07",
                "commit": "e" * 40,
                "archive_url": "https://example.invalid/u-boot.tar.bz2",
                "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "license": "GPL-2.0-only",
            },
        }

    @staticmethod
    def full_uboot_layout() -> dict[str, int]:
        """Provide the memory layout independently expected by the producer checks."""
        return {
            "uboot_load": 0x81000000,
            "uboot_size": 0x00100000,
            "uboot_stack": 0x80F00000,
            "fit_load": 0x83200000,
            "fdt_pad": 0x00003000,
        }

    @staticmethod
    def prepare_synthetic_source(source: Path) -> None:
        """Install the explicit fixture programs that model the external source tree."""
        (source / "configs").mkdir(parents=True)
        scripts = source / "scripts"
        scripts.mkdir()
        shutil.copyfile(UBOOT_FIXTURES / "Makefile", source / "Makefile")
        for name in ("fake-build.py", "tool-version.py"):
            shutil.copyfile(UBOOT_FIXTURES / name, scripts / name)

    def _build(self, *, jobs: int = 1) -> uboot_tools.UbootBuild:
        return uboot_tools.build_full(
            self.archive,
            self.config,
            defconfig=self.defconfig,
            projections=[(self.projection, "scripts/build-log.sh")],
            patches=[],
            work=self.work,
            jobs=jobs,
            container_recipe="a" * 64,
            cross_compile="arm-linux-gnueabi-",
            layout=self.layout,
        )

    def _build_log_lines(self) -> list[str]:
        if not self.build_log.exists():
            return []
        return self.build_log.read_text(encoding="utf-8").splitlines()

    def test_full_output_is_reused_across_jobs_and_unrelated_siblings(self) -> None:
        """A complete verified output serves the same recipe without another build."""
        first = self._build()
        first_binary = first.binary.read_bytes()

        self.assertEqual(first.elf.read_bytes()[:4], b"\x7fELF")
        self.assertEqual(first.config.read_text(encoding="utf-8"), self.required_config)
        self.assertEqual(self._build_log_lines(), ["build"])

        (self.root / "unrelated-sibling.txt").write_text("unchanged input\n", encoding="utf-8")
        reused = self._build(jobs=8)

        self.assertEqual(reused.receipt, first.receipt)
        self.assertEqual(reused.binary.read_bytes(), first_binary)
        self.assertEqual(self._build_log_lines(), ["build"])

    def test_defconfig_miss_and_invalid_cached_outputs_rebuild(self) -> None:
        """One declared input changes the result, while missing or altered outputs revoke reuse."""
        first = self._build()
        self.defconfig.write_text(
            self.required_config.replace("CONFIG_TEST_INPUT=one", "CONFIG_TEST_INPUT=two"),
            encoding="utf-8",
        )

        changed = self._build()
        self.assertNotEqual(changed.receipt, first.receipt)
        self.assertIn("CONFIG_TEST_INPUT=two\n", changed.config.read_text(encoding="utf-8"))
        self.assertEqual(self._build_log_lines(), ["build", "build"])

        changed.binary.write_bytes(b"tampered")
        rebuilt = self._build()
        self.assertEqual(rebuilt.receipt, changed.receipt)
        self.assertEqual(rebuilt.binary.read_bytes(), b"binary")
        self.assertEqual(self._build_log_lines(), ["build", "build", "build"])

        rebuilt.map.unlink()
        restored = self._build()
        self.assertEqual(restored.receipt, changed.receipt)
        self.assertTrue(restored.map.is_file())
        self.assertEqual(self._build_log_lines(), ["build", "build", "build", "build"])

    def test_failed_changed_recipe_preserves_previous_complete_output_and_receipt(self) -> None:
        """A failed replacement leaves every last-good output and its receipt intact."""
        current = self._build()
        visible_outputs = {
            "elf": current.elf,
            "binary": current.binary,
            "dtb": current.dtb,
            "map": current.map,
            "config": current.config,
            "mkimage": current.mkimage,
            "dumpimage": current.dumpimage,
            "receipt": self.work / "uboot" / uboot_tools.RECEIPT_NAME,
        }
        before = {name: path.read_bytes() for name, path in visible_outputs.items()}
        self.defconfig.write_text(
            self.required_config.replace("CONFIG_TEST_INPUT=one", "CONFIG_TEST_FAIL=y"),
            encoding="utf-8",
        )

        with self.assertRaises(subprocess.CalledProcessError):
            self._build()

        after = {name: path.read_bytes() for name, path in visible_outputs.items()}
        self.assertEqual(after, before)
        self.assertEqual(self._build_log_lines(), ["build"])

    def test_writable_or_unverified_config_cannot_replace_complete_output(self) -> None:
        """The producer rejects unsafe compiled configs before publishing them."""
        current = self._build()
        before = current.config.read_bytes()
        for previous, unsafe in (
            ("# CONFIG_MMC_WRITE is not set", "CONFIG_MMC_WRITE=y"),
            ("CONFIG_ENV_IS_NOWHERE=y", "CONFIG_ENV_IS_IN_FAT=y"),
            ("CONFIG_FIT_FULL_CHECK=y", "# CONFIG_FIT_FULL_CHECK is not set"),
        ):
            with self.subTest(config=unsafe):
                self.defconfig.write_text(self.required_config.replace(previous, unsafe))
                with self.assertRaisesRegex(uboot_tools.UbootToolsError, "read-only MMC contract"):
                    self._build()
                self.assertEqual(current.config.read_bytes(), before)

    def test_profile_preparation_emits_the_selected_target_identity(self) -> None:
        """A C preprocessor in the synthetic build consumes the chosen target name."""
        layout_prefixes = (
            "CONFIG_TEXT_BASE=",
            "CONFIG_CUSTOM_SYS_INIT_SP_ADDR=",
            "CONFIG_SYS_LOAD_ADDR=",
            "CONFIG_SYS_FDT_PAD=",
        )
        base = "\n".join(
            line
            for line in self.required_config.splitlines()
            if not line.startswith(layout_prefixes)
        )
        receipts = []
        for target, defconfig in (
            ("nokia-ta1618", "ta1618_defconfig"),
            ("inoi-240-modern-4g", "inoi240_defconfig"),
            ("inoi-244-modern-4g", "inoi244_defconfig"),
        ):
            source = self.root / "targets" / target / "uboot" / defconfig
            source.parent.mkdir(parents=True)
            source.write_text(base)
            target_config = {
                "profile": "microsd-uboot",
                "layout": {
                    **self.layout,
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
                    "fit_size": 0x00C00000,
                    "resident_start": 0x80100000,
                    "resident_limit": 0x81000000,
                },
                "uboot": {
                    **self.config,
                    "defconfig": f"uboot/{defconfig}",
                    "patches": [],
                    "copies": [{"source": "build-log.sh", "destination": "scripts/build-log.sh"}],
                },
            }
            with (
                self.subTest(target=target),
                mock.patch.object(common, "ROOT", self.root),
                mock.patch.object(sources_build, "fetch", return_value=self.archive),
                mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "a" * 64}),
            ):
                built = storage_build.build_profile_uboot(target, target_config, self.work, 1)
                if built is None:
                    self.fail("configured U-Boot build did not produce an artifact")
                self.assertEqual(built.dtb.read_bytes().strip(), f'"{target}"'.encode("ascii"))
                receipts.append(built.receipt["recipe"])
        self.assertEqual(len(set(receipts)), 3)


if __name__ == "__main__":
    unittest.main()

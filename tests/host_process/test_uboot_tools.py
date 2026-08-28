# SPDX-License-Identifier: GPL-2.0-only
"""Cache-lifecycle tests with a synthetic make-compatible U-Boot source."""

from __future__ import annotations

import hashlib
import subprocess
import tarfile
import tempfile
import textwrap
import unittest
from pathlib import Path

from fplinux_cli import uboot_tools


class UbootToolsTests(unittest.TestCase):
    """Exercise the producer without claiming a real U-Boot build."""

    _REQUIRED_CONFIG = textwrap.dedent(
        """\
        CONFIG_TARGET_FPLINUX_TA1618=y
        CONFIG_TEXT_BASE=0x81000000
        CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x80f00000
        CONFIG_SYS_LOAD_ADDR=0x83200000
        CONFIG_SYS_FDT_PAD=0x00003000
        CONFIG_ENV_IS_NOWHERE=y
        CONFIG_AUTOBOOT=y
        CONFIG_BOOTDELAY=-2
        CONFIG_USE_BOOTCOMMAND=y
        CONFIG_BOOTCOMMAND=\"sdboot\"
        # CONFIG_BOOTSTD is not set
        CONFIG_SYS_DCACHE_OFF=y
        CONFIG_FIT=y
        CONFIG_FIT_FULL_CHECK=y
        CONFIG_SHA256=y
        CONFIG_LMB=y
        # CONFIG_FIT_SIGNATURE is not set
        # CONFIG_LEGACY_IMAGE_FORMAT is not set
        CONFIG_CMD_BOOTM=y
        CONFIG_MMC=y
        CONFIG_DM_MMC=y
        # CONFIG_CMD_MMC is not set
        # CONFIG_MMC_WRITE is not set
        # CONFIG_MMC_HW_PARTITIONING is not set
        # CONFIG_CMD_FAT is not set
        # CONFIG_CMD_FS_GENERIC is not set
        CONFIG_FS_FAT=y
        CONFIG_DOS_PARTITION=y
        # CONFIG_FAT_WRITE is not set
        # CONFIG_BLOCK_CACHE is not set
        # CONFIG_USB is not set
        # CONFIG_NET is not set
        # CONFIG_EFI_LOADER is not set
        CONFIG_TEST_INPUT=one
        """
    )

    def setUp(self) -> None:
        """Create the make-compatible external source boundary used by every case."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        source = self.root / "source/u-boot-2026.07"
        (source / "configs").mkdir(parents=True)
        (source / "scripts").mkdir()
        (source / "scripts/fake-build.py").write_text(
            textwrap.dedent(
                """\
                #!/usr/bin/env python3
                import pathlib
                import sys

                output = pathlib.Path(sys.argv[1])
                output.mkdir(parents=True, exist_ok=True)
                elf = bytearray(52)
                elf[0:4] = b"\\x7fELF"
                elf[4:7] = b"\\x01\\x01\\x01"
                elf[16:18] = (2).to_bytes(2, "little")
                elf[18:20] = (40).to_bytes(2, "little")
                elf[20:24] = (1).to_bytes(4, "little")
                elf[24:28] = (0x81000000).to_bytes(4, "little")
                (output / "u-boot").write_bytes(bytes(elf) + b"DEBUG")
                (output / "u-boot-dtb.bin").write_bytes(b"binary")
                (output / "u-boot.dtb").write_bytes(b"dtb")
                (output / "u-boot.map").write_text(
                    (output / ".config").read_text(encoding="utf-8"),
                    encoding="utf-8",
                )
                tools = output / "tools"
                tools.mkdir()
                for name in ("mkimage", "dumpimage"):
                    tool = tools / name
                    tool.write_text(
                        '#!/bin/sh\\n'
                        'test "$1" = -V || exit 1\\n'
                        f'echo "{name} version 2026.07"\\n',
                        encoding="utf-8",
                    )
                    tool.chmod(0o755)
                """
            ),
            encoding="utf-8",
        )
        (source / "Makefile").write_text(
            textwrap.dedent(
                """\
                .PHONY: ta1618_defconfig all
                ta1618_defconfig:
                \t@mkdir -p "$(O)"
                \t@cp configs/ta1618_defconfig "$(O)/.config"
                all:
                \t@if grep -qx 'CONFIG_TEST_FAIL=y' "$(O)/.config"; then exit 42; fi
                \t@sh scripts/build-log.sh
                \t@python3 scripts/fake-build.py "$(O)"
                """
            ),
            encoding="utf-8",
        )
        self.defconfig = self.root / "ta1618_defconfig"
        self.defconfig.write_text(self._REQUIRED_CONFIG, encoding="utf-8")
        self.build_log = self.root / "build.log"
        self.projection = self.root / "build-log.sh"
        self.projection.write_text(
            f"printf '%s\\n' build >> {self.build_log}\n",
            encoding="utf-8",
        )
        self.archive = self.root / "source.tar.bz2"
        unpacked = self.root / "source.tar"
        with tarfile.open(unpacked, "w:") as output:
            output.add(source, arcname="u-boot-2026.07")
        with self.archive.open("wb") as output:
            subprocess.run(
                ["bzip2", "-c", str(unpacked)],
                stdout=output,
                check=True,
                timeout=30,
            )
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.config = {
            "kind": "full",
            "source": "u-boot.lock.toml",
            "archive_prefix": "u-boot-2026.07",
            "lock": {
                "version": "2026.07",
                "repository": "https://source.denx.de/u-boot/u-boot.git",
                "tag": "v2026.07",
                "commit": "e" * 40,
                "archive_url": "https://example.invalid/u-boot.tar.bz2",
                "archive_sha256": digest,
                "license": "GPL-2.0-only",
            },
        }
        self.work = self.root / "work"
        self.layout = {
            "uboot_load": 0x81000000,
            "uboot_size": 0x00100000,
            "uboot_stack": 0x80F00000,
            "fit_load": 0x83200000,
            "fdt_pad": 0x00003000,
        }

    def _build(self, *, jobs: int = 1) -> uboot_tools.UbootBuild:
        return uboot_tools.build_full(
            self.archive,
            self.config,
            self.defconfig,
            [(self.projection, "scripts/build-log.sh")],
            [],
            self.work,
            jobs,
            "a" * 64,
            "arm-linux-gnueabi-",
            self.layout,
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
        self.assertEqual(first.config.read_text(encoding="utf-8"), self._REQUIRED_CONFIG)
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
            self._REQUIRED_CONFIG.replace("CONFIG_TEST_INPUT=one", "CONFIG_TEST_INPUT=two"),
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
            self._REQUIRED_CONFIG.replace("CONFIG_TEST_INPUT=one", "CONFIG_TEST_FAIL=y"),
            encoding="utf-8",
        )

        with self.assertRaises(subprocess.CalledProcessError):
            self._build()

        after = {name: path.read_bytes() for name, path in visible_outputs.items()}
        self.assertEqual(after, before)
        self.assertEqual(self._build_log_lines(), ["build"])


if __name__ == "__main__":
    unittest.main()

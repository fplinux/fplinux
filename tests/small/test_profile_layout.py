# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for microSD profile layout rendering."""

from __future__ import annotations

import unittest
from typing import ClassVar

from fplinux_cli import profile_layout


class ProfileLayoutRenderTests(unittest.TestCase):
    """Keep generated inputs consistent with their selected layout values."""

    layout: ClassVar[dict[str, int]] = {
        "ram_base": 0x80000000,
        "ram_size": 0x04000000,
        "timer_hz": 1000,
        "resident_start": 0x80100000,
        "resident_limit": 0x81000000,
        "uboot_load": 0x81000000,
        "uboot_size": 0x00100000,
        "uboot_stack": 0x80F00000,
        "kernel_load": 0x82000000,
        "kernel_entry": 0x82000000,
        "kernel_size": 0x01200000,
        "fit_load": 0x83200000,
        "fit_size": 0x00C00000,
        "fdt_load": 0x83E00000,
        "fdt_size": 0x00010000,
        "fdt_pad": 0x00003000,
        "framebuffer": 0x83F00000,
        "framebuffer_size": 0x00100000,
    }

    @staticmethod
    def _defines(contents: bytes) -> dict[str, int]:
        """Read C preprocessor values without tying the test to file formatting."""
        definitions: dict[str, int] = {}
        for line in contents.decode("ascii").splitlines():
            if not line.startswith("#define FPLINUX_BOOT_LAYOUT_"):
                continue
            fields = line.split()
            if len(fields) != 3:
                continue
            _directive, name, value = fields
            key = name.removeprefix("FPLINUX_BOOT_LAYOUT_")
            definitions[key] = int(value.removesuffix("U"), 16)
        return definitions

    @staticmethod
    def _config(contents: bytes) -> dict[str, str]:
        """Read Kconfig assignments independently of their output order."""
        return {
            name: value
            for line in contents.decode("ascii").splitlines()
            if "=" in line
            for name, value in (line.split("=", 1),)
        }

    def test_bootstrap_and_uboot_inputs_expose_selected_layout_values(self) -> None:
        """Generated C, linker and U-Boot inputs carry the declared RAM placement."""
        header = profile_layout.boot_layout_header(self.layout)
        self.assertEqual(
            self._defines(header),
            {
                "RAM_BASE_PHYS": 0x80000000,
                "RAM_REQUIRED_BYTES": 0x04000000,
                "RAM_LIMIT_PHYS": 0x84000000,
                "TIMER_HZ": 1000,
                "ZIMAGE_PHYS": 0x82000000,
                "ZIMAGE_ENTRY_PHYS": 0x82000000,
                "ZIMAGE_LIMIT_BYTES": 0x01200000,
                "DTB_PHYS": 0x83E00000,
                "DTB_LIMIT_BYTES": 0x00010000,
                "FRAMEBUFFER_PHYS": 0x83F00000,
                "FRAMEBUFFER_BYTES": 0x00100000,
                "RESIDENT_START_PHYS": 0x80100000,
                "RESIDENT_LIMIT_PHYS": 0x81000000,
                "UBOOT_LOAD_PHYS": 0x81000000,
                "UBOOT_LIMIT_BYTES": 0x00100000,
                "UBOOT_STACK_PHYS": 0x80F00000,
                "FIT_PHYS": 0x83200000,
                "FIT_LIMIT_BYTES": 0x00C00000,
            },
        )
        self.assertEqual(
            profile_layout.bootstrap_memory_ld(
                {"load_address": 0x80100000, "payload_limit": 0x81000000},
                self.layout,
            )
            .decode("ascii")
            .splitlines(),
            [
                "EXTRA_START = 0x80000000;",
                "EXTRA_SIZE = 0x00100000;",
                "IMAGE_START = 0x80100000;",
                "IMAGE_SIZE = 0x00f00000;",
            ],
        )
        dtsi = profile_layout.uboot_layout_dtsi(self.layout).decode("ascii")
        self.assertIn("#define FPLINUX_UBOOT_TIMER_HZ 1000", dtsi)
        self.assertIn("memory@80000000", dtsi)
        self.assertIn("reg = <0x80000000 0x04000000>;", dtsi)
        self.assertEqual(
            self._config(
                profile_layout.uboot_defconfig(
                    b"CONFIG_ENV_IS_NOWHERE=y\n",
                    self.layout,
                )
            ),
            {
                "CONFIG_ENV_IS_NOWHERE": "y",
                "CONFIG_TEXT_BASE": "0x81000000",
                "CONFIG_CUSTOM_SYS_INIT_SP_ADDR": "0x80f00000",
                "CONFIG_SYS_LOAD_ADDR": "0x83200000",
                "CONFIG_SYS_FDT_PAD": "0x00003000",
            },
        )

    def test_external_root_and_image_inputs_reference_declared_storage(self) -> None:
        """Generated bootargs and genimage input select the intended card partitions."""
        root = {
            "kind": "external",
            "filesystem": "ext4",
            "partuuid": "46504c58-02",
            "wait_seconds": 10,
        }
        fit = {"filename": "FPLINUX.ITB"}
        storage = {
            "filename": "FPLINUX.img",
            "disk_signature": 0x46504C58,
            "boot_offset": 0x00100000,
            "boot_size": 0x04000000,
            "boot_label": "FPLBOOT",
            "root_offset": 0x04100000,
            "root_size": 0x04000000,
            "root_filename": "FPLROOT.ext4",
            "root_label": "FPLROOT",
        }

        bootargs = profile_layout.external_root_dtsi(root).decode("ascii")
        self.assertTrue(bootargs.startswith('bootargs = "'))
        self.assertTrue(bootargs.endswith('";\n'))
        for argument in (
            "root=PARTUUID=46504c58-02",
            "rootfstype=ext4",
            "rootwait=10",
            "rw",
            "init=/sbin/init",
        ):
            with self.subTest(argument=argument):
                self.assertIn(argument, bootargs)

        image = profile_layout.genimage_config(fit, storage).decode("ascii")
        for value in (
            "image FPLINUX.img {",
            'partition-table-type = "mbr"',
            "disk-signature = 0x46504c58",
            "partition-type = 0x0c",
            "offset = 1048576",
            "size = 67108864",
            "partition-type = 0x83",
            'image = "FPLROOT.ext4"',
            "offset = 68157440",
            "fill = true",
            'label = "FPLBOOT"',
            "file FPLINUX.ITB {",
        ):
            with self.subTest(value=value):
                self.assertIn(value, image)

    def test_base_defconfig_cannot_override_generated_profile_placement(self) -> None:
        """Reject a second source of U-Boot text, stack or FDT placement."""
        with self.assertRaisesRegex(ValueError, "profile-owned layout values"):
            profile_layout.uboot_defconfig(
                b"CONFIG_SYS_FDT_PAD=0x1000\n",
                self.layout,
            )


if __name__ == "__main__":
    unittest.main()

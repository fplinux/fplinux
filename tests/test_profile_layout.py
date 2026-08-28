# SPDX-License-Identifier: GPL-2.0-only
"""Exact transient-layout renderer tests for the microSD build artifacts."""

from __future__ import annotations

import unittest
from typing import ClassVar

from fplinux_cli import profile_layout


class ProfileLayoutRenderTests(unittest.TestCase):
    """Keep the generated build inputs tied to one declared layout contract."""

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

    def test_bootstrap_header_and_uboot_inputs_render_exact_selected_addresses(self) -> None:
        """Render the shared C header plus the U-Boot RAM and FDT placement inputs."""
        expected_header = b"""/* SPDX-License-Identifier: GPL-2.0-only */
/* Generated from the selected build profile. */
#ifndef FPLINUX_BOOT_LAYOUT_H
#define FPLINUX_BOOT_LAYOUT_H

#define FPLINUX_BOOT_LAYOUT_RAM_BASE_PHYS 0x80000000U
#define FPLINUX_BOOT_LAYOUT_RAM_REQUIRED_BYTES 0x04000000U
#define FPLINUX_BOOT_LAYOUT_RAM_LIMIT_PHYS 0x84000000U
#define FPLINUX_BOOT_LAYOUT_TIMER_HZ 0x000003e8U
#define FPLINUX_BOOT_LAYOUT_ZIMAGE_PHYS 0x82000000U
#define FPLINUX_BOOT_LAYOUT_ZIMAGE_ENTRY_PHYS 0x82000000U
#define FPLINUX_BOOT_LAYOUT_ZIMAGE_LIMIT_BYTES 0x01200000U
#define FPLINUX_BOOT_LAYOUT_DTB_PHYS 0x83e00000U
#define FPLINUX_BOOT_LAYOUT_DTB_LIMIT_BYTES 0x00010000U
#define FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_PHYS 0x83f00000U
#define FPLINUX_BOOT_LAYOUT_FRAMEBUFFER_BYTES 0x00100000U
#define FPLINUX_BOOT_LAYOUT_RESIDENT_START_PHYS 0x80100000U
#define FPLINUX_BOOT_LAYOUT_RESIDENT_LIMIT_PHYS 0x81000000U
#define FPLINUX_BOOT_LAYOUT_UBOOT_LOAD_PHYS 0x81000000U
#define FPLINUX_BOOT_LAYOUT_UBOOT_LIMIT_BYTES 0x00100000U
#define FPLINUX_BOOT_LAYOUT_UBOOT_STACK_PHYS 0x80f00000U
#define FPLINUX_BOOT_LAYOUT_FIT_PHYS 0x83200000U
#define FPLINUX_BOOT_LAYOUT_FIT_LIMIT_BYTES 0x00c00000U

#endif
"""
        expected_dtsi = b"""#define FPLINUX_UBOOT_TIMER_HZ 1000
memory@80000000 {
\tdevice_type = "memory";
\treg = <0x80000000 0x04000000>;
};
"""
        expected_defconfig = b"""CONFIG_ENV_IS_NOWHERE=y
CONFIG_TEXT_BASE=0x81000000
CONFIG_CUSTOM_SYS_INIT_SP_ADDR=0x80f00000
CONFIG_SYS_LOAD_ADDR=0x83200000
CONFIG_SYS_FDT_PAD=0x00003000
"""

        self.assertEqual(profile_layout.boot_layout_header(self.layout), expected_header)
        self.assertEqual(
            profile_layout.bootstrap_memory_ld(
                {"load_address": 0x80100000, "payload_limit": 0x81000000},
                self.layout,
            ),
            b"""EXTRA_START = 0x80000000;
EXTRA_SIZE = 0x00100000;
IMAGE_START = 0x80100000;
IMAGE_SIZE = 0x00f00000;
""",
        )
        self.assertEqual(profile_layout.uboot_layout_dtsi(self.layout), expected_dtsi)
        self.assertEqual(
            profile_layout.uboot_defconfig(b"CONFIG_ENV_IS_NOWHERE=y\n", self.layout),
            expected_defconfig,
        )

    def test_external_root_and_card_image_render_the_declared_storage_contract(self) -> None:
        """Render the independently consumable Linux bootargs and genimage input exactly."""
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
        expected_root = (
            b'bootargs = "console=tty0 loglevel=8 ignore_loglevel '
            b"root=PARTUUID=46504c58-02 rootfstype=ext4 rootwait=10 rw "
            b"init=/sbin/init panic=-1 vt.global_cursor_default=1 "
            b'random.trust_bootloader=on";\n'
        )
        expected_image = b"""# SPDX-License-Identifier: GPL-2.0-only
image FPLINUX.img {
\thdimage {
\t\talign = 1M
\t\tpartition-table-type = "mbr"
\t\tdisk-signature = 0x46504c58
\t}

\tpartition boot {
\t\tpartition-type = 0x0c
\t\timage = "FPLINUX.vfat"
\t\toffset = 1048576
\t\tsize = 67108864
\t}

\tpartition root {
\t\tpartition-type = 0x83
\t\timage = "FPLROOT.ext4"
\t\toffset = 68157440
\t\tsize = 67108864
\t\tfill = true
\t}
}

image FPLINUX.vfat {
\ttemporary = true
\tsize = 67108864

\tvfat {
\t\tlabel = "FPLBOOT"
\t\textraargs = "-F 32 --invariant -i 46504c58"

\t\tfile FPLINUX.ITB {
\t\t\timage = "FPLINUX.ITB"
\t\t}
\t}
}
"""

        self.assertEqual(profile_layout.external_root_dtsi(root), expected_root)
        self.assertEqual(profile_layout.genimage_config(fit, storage), expected_image)

    def test_base_defconfig_cannot_override_the_generated_profile_placement(self) -> None:
        """Reject a second source of the U-Boot text, stack or FDT placement."""
        with self.assertRaisesRegex(ValueError, "profile-owned layout values"):
            profile_layout.uboot_defconfig(
                b"CONFIG_SYS_FDT_PAD=0x1000\n",
                self.layout,
            )


if __name__ == "__main__":
    unittest.main()

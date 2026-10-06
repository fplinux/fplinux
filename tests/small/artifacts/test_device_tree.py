# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral tests for binary FDT identity verification."""

from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    exact_path_properties,
    parse_nul_string,
    parse_nul_string_list,
    verify_dtb_kconfig,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    verify_target_identity,
)
from fplinux_cli.manifests.platforms import load_platform

from tests.fdt import binary_tree


class DeviceTreeKconfigTests(unittest.TestCase):
    """Check compiled USB ownership against independent literal configuration values."""

    checks: ClassVar[tuple[dict[str, str], ...]] = (
        {
            "path": "/soc/usb@20200000",
            "compatible": "sprd,ums9117-musb",
            "config": "CONFIG_USB_MUSB_UMS9117_COLD",
        },
        {
            "path": "/soc/usb@20200000",
            "compatible": "fplinux,ums9117-musb-inherited",
            "config": "CONFIG_USB_MUSB_UMS9117_INHERITED",
        },
    )

    @staticmethod
    def usb_tree(compatible: bytes, status: bytes | None = b"okay\0") -> bytes:
        """Encode USB properties using the test-owned FDT format fixture."""
        properties = [("compatible", compatible)]
        if status is not None:
            properties.append(("status", status))
        return binary_tree([], [("soc", [], [("usb@20200000", properties, [])])])

    def test_each_enabled_ownership_path_requires_its_built_in_driver(self) -> None:
        """A cold owner cannot satisfy inherited DT ownership, or the reverse."""
        cases = (
            (b"sprd,ums9117-musb\0", "CONFIG_USB_MUSB_UMS9117_COLD"),
            (b"fplinux,ums9117-musb-inherited\0", "CONFIG_USB_MUSB_UMS9117_INHERITED"),
        )
        for compatible, symbol in cases:
            with self.subTest(compatible=compatible):
                tree = self.usb_tree(compatible)
                verify_dtb_kconfig(tree, {symbol: "y"}, self.checks)
                for value in ("n", "m", None):
                    config = {symbol: value} if value is not None else {}
                    with (
                        self.subTest(value=value),
                        self.assertRaisesRegex(DeviceTreeError, f"requires {symbol}=y"),
                    ):
                        verify_dtb_kconfig(tree, config, self.checks)

    def test_other_usb_owner_does_not_satisfy_enabled_node(self) -> None:
        """Built cold support still rejects an inherited node without inherited support."""
        with self.assertRaisesRegex(DeviceTreeError, "CONFIG_USB_MUSB_UMS9117_INHERITED=y"):
            verify_dtb_kconfig(
                self.usb_tree(b"fplinux,ums9117-musb-inherited\0"),
                {"CONFIG_USB_MUSB_UMS9117_COLD": "y"},
                self.checks,
            )

    def test_disabled_node_needs_no_built_ownership(self) -> None:
        """Disabled USB can remain described while neither ownership path is built."""
        verify_dtb_kconfig(self.usb_tree(b"sprd,ums9117-musb\0", b"disabled\0"), {}, self.checks)

    def test_absent_or_ok_status_enables_the_node(self) -> None:
        """Standard DT availability spellings enforce ownership equally."""
        for status in (None, b"ok\0"):
            with (
                self.subTest(status=status),
                self.assertRaisesRegex(DeviceTreeError, "requires CONFIG_USB_MUSB_UMS9117_COLD=y"),
            ):
                verify_dtb_kconfig(self.usb_tree(b"sprd,ums9117-musb\0", status), {}, self.checks)

    def test_enabled_unknown_compatible_is_rejected(self) -> None:
        """An enabled USB node cannot bypass ownership by changing its compatible."""
        with self.assertRaisesRegex(DeviceTreeError, "has no declared Kconfig owner"):
            verify_dtb_kconfig(self.usb_tree(b"example,usb\0"), {}, self.checks)

    def test_compatible_fallback_list_preserves_known_owner_requirement(self) -> None:
        """A secondary generic compatible does not replace the platform owner."""
        tree = self.usb_tree(b"sprd,ums9117-musb\0example,generic-musb\0")
        verify_dtb_kconfig(tree, {"CONFIG_USB_MUSB_UMS9117_COLD": "y"}, self.checks)


class DeviceTreePropertyKconfigTests(unittest.TestCase):
    """Validate optional keypad support through independently encoded DT properties."""

    checks: ClassVar[tuple[dict[str, str], ...]] = (
        {
            "path": "/soc/keypad@40250000",
            "compatible": "sprd,ums9117-keypad",
            "config": "CONFIG_KEYBOARD_UMS9117",
        },
        {
            "path": "/soc/keypad@40250000",
            "compatible": "sprd,ums9117-keypad",
            "property": "eic9-gpios",
            "config": "CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY",
        },
    )

    @staticmethod
    def keypad_tree(*, has_aux_key: bool, status: bytes = b"okay\0") -> bytes:
        """Encode a keypad with an optional auxiliary GPIO using the test-owned FDT fixture."""
        properties = [("compatible", b"sprd,ums9117-keypad\0"), ("status", status)]
        if has_aux_key:
            properties.append(("eic9-gpios", struct.pack(">III", 1, 9, 0)))
        return binary_tree([], [("soc", [], [("keypad@40250000", properties, [])])])

    def test_aux_gpio_requires_its_built_in_support(self) -> None:
        """A declared auxiliary key cannot be silently omitted from an enabled keypad."""
        tree = self.keypad_tree(has_aux_key=True)
        for value in ("n", "m", None):
            config = {"CONFIG_KEYBOARD_UMS9117": "y"}
            if value is not None:
                config["CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY"] = value
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    DeviceTreeError,
                    "property eic9-gpios requires CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY=y",
                ),
            ):
                verify_dtb_kconfig(tree, config, self.checks)
        verify_dtb_kconfig(
            tree,
            {"CONFIG_KEYBOARD_UMS9117": "y", "CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY": "y"},
            self.checks,
        )

    def test_platform_requirements_reject_an_omitted_aux_key(self) -> None:
        """The platform's declared policy rejects a keypad whose auxiliary key is not built."""
        tree = binary_tree(
            [],
            [
                (
                    "soc",
                    [],
                    [
                        ("usb@20200000", [("compatible", b"sprd,ums9117-musb\0")], []),
                        (
                            "keypad@40250000",
                            [
                                ("compatible", b"sprd,ums9117-keypad\0"),
                                ("eic9-gpios", struct.pack(">III", 1, 9, 0)),
                            ],
                            [],
                        ),
                    ],
                ),
            ],
        )
        checks = load_platform("ums9117")["linux"]["dt_config_checks"]
        with self.assertRaisesRegex(DeviceTreeError, "CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY=y"):
            verify_dtb_kconfig(
                tree,
                {
                    "CONFIG_USB_MUSB_UMS9117_COLD": "y",
                    "CONFIG_KEYBOARD_UMS9117": "y",
                    "CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY": "n",
                },
                checks,
            )

    def test_absent_aux_gpio_needs_only_the_keypad_driver(self) -> None:
        """Targets without an auxiliary GPIO keep that feature absent from their kernel."""
        verify_dtb_kconfig(
            self.keypad_tree(has_aux_key=False), {"CONFIG_KEYBOARD_UMS9117": "y"}, self.checks
        )

    def test_aux_support_does_not_replace_the_keypad_driver(self) -> None:
        """Conditional support cannot satisfy the device's unconditional driver requirement."""
        with self.assertRaisesRegex(DeviceTreeError, "requires CONFIG_KEYBOARD_UMS9117=y"):
            verify_dtb_kconfig(
                self.keypad_tree(has_aux_key=True),
                {"CONFIG_KEYBOARD_UMS9117_AUX_EIC_KEY": "y"},
                self.checks,
            )

    def test_disabled_aux_keypad_needs_no_built_support(self) -> None:
        """A disabled keypad imposes no driver or auxiliary-feature requirement."""
        verify_dtb_kconfig(
            self.keypad_tree(has_aux_key=True, status=b"disabled\0"), {}, self.checks
        )


def _binary_profile_layout_tree(
    *,
    memory_base: int = 0x80000000,
    memory_size: int = 0x03E00000,
    reserved_range: tuple[int, int] = (0x83F00000, 0x00100000),
    reserved_no_map: bool = True,
) -> bytes:
    """Construct boot-memory properties with an independent display DMA pool."""
    reserved_properties = [("reg", struct.pack(">II", *reserved_range))]
    if reserved_no_map:
        reserved_properties.append(("no-map", b""))
    return binary_tree(
        [],
        [
            (
                "chosen",
                [
                    (
                        "bootargs",
                        (
                            b"root=PARTUUID=46504c58-02 rootfstype=ext4 "
                            b"rootwait=10 rw init=/sbin/init\0"
                        ),
                    ),
                ],
                [],
            ),
            (
                f"memory@{memory_base:x}",
                [
                    ("device_type", b"memory\0"),
                    ("reg", struct.pack(">II", memory_base, memory_size)),
                ],
                [],
            ),
            (
                "reserved-memory",
                [
                    ("#address-cells", struct.pack(">I", 1)),
                    ("#size-cells", struct.pack(">I", 1)),
                    ("ranges", b""),
                ],
                [
                    (
                        "framebuffer@83f00000",
                        reserved_properties,
                        [],
                    ),
                    (
                        "codec-dma-pool",
                        [
                            ("compatible", b"shared-dma-pool\0"),
                            ("reusable", b""),
                            ("size", struct.pack(">I", 0x00400000)),
                            ("phandle", struct.pack(">I", 1)),
                        ],
                        [],
                    ),
                ],
            ),
            (
                "soc",
                [],
                [
                    (
                        "display@20800000",
                        [
                            ("reg", struct.pack(">II", 0x20800000, 0x1000)),
                            ("reg-names", b"lcdc\0"),
                            ("memory-region", struct.pack(">I", 1)),
                        ],
                        [],
                    )
                ],
            ),
        ],
    )


class DeviceTreePropertyTests(unittest.TestCase):
    """Read properties from exact paths in binary FDT fixtures."""

    def test_exact_paths_do_not_mix_properties_from_different_nodes(self) -> None:
        """The same property name at root and child retains path ownership."""
        tree = binary_tree(
            [("compatible", b"vendor,board\0")],
            [
                (
                    "chosen",
                    [("compatible", b"fplinux,session\0"), ("bootargs", b"x\0")],
                    [],
                )
            ],
        )

        properties = exact_path_properties(tree, ("/", "/chosen"))

        self.assertEqual(properties["/"]["compatible"], b"vendor,board\0")
        self.assertEqual(properties["/chosen"]["compatible"], b"fplinux,session\0")
        self.assertEqual(properties["/chosen"]["bootargs"], b"x\0")

    def test_missing_exact_path_is_rejected(self) -> None:
        """A similarly named property cannot substitute for the requested node."""
        tree = binary_tree([("model", b"Demo\0")])

        with self.assertRaisesRegex(DeviceTreeError, r"lacks node /chosen"):
            exact_path_properties(tree, "/chosen")

    def test_nul_string_parsers_reject_ambiguous_encodings(self) -> None:
        """Missing terminators and empty string-list members are not accepted."""
        with self.assertRaisesRegex(DeviceTreeError, "one NUL-terminated string"):
            parse_nul_string(b"Demo\0extra\0", "model")
        with self.assertRaisesRegex(DeviceTreeError, "NUL-terminated string-list"):
            parse_nul_string_list(b"vendor,board", "compatible")
        with self.assertRaisesRegex(DeviceTreeError, "empty string-list element"):
            parse_nul_string_list(b"vendor,board\0\0", "compatible")


class TargetIdentityTests(unittest.TestCase):
    """Verify target identity against observable root properties in a DTB."""

    target = "demo-target"
    model = "Demo Phone (D-1)"
    compatibles = ("vendor,demo-phone", "vendor,demo-soc")

    def tree(
        self,
        *,
        model: bytes | None = b"Demo Phone (D-1)\0",
        compatible: bytes | None = b"vendor,demo-phone\0vendor,demo-soc\0",
    ) -> bytes:
        """Build a root identity while allowing individual properties to be omitted."""
        properties: list[tuple[str, bytes]] = []
        if model is not None:
            properties.append(("model", model))
        if compatible is not None:
            properties.append(("compatible", compatible))
        return binary_tree(properties)

    def test_matching_identity_is_accepted_from_a_binary_fdt_path(self) -> None:
        """The verifier reads and accepts matching binary FDT properties."""
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "target.dtb"
            path.write_bytes(self.tree())

            verify_target_identity(path, self.target, self.model, self.compatibles)

    def test_model_mismatch_reports_the_target_and_both_values(self) -> None:
        """A different device model fails even when compatibles still match."""
        with self.assertRaisesRegex(
            DeviceTreeError,
            r"demo-target DTB model mismatch: expected 'Demo Phone \(D-1\)', got 'Other'",
        ):
            verify_target_identity(
                self.tree(model=b"Other\0"),
                self.target,
                self.model,
                self.compatibles,
            )

    def test_compatible_order_is_part_of_the_identity(self) -> None:
        """SoC-first fallback ordering cannot pass a target-first contract."""
        with self.assertRaisesRegex(DeviceTreeError, "compatible mismatch"):
            verify_target_identity(
                self.tree(compatible=b"vendor,demo-soc\0vendor,demo-phone\0"),
                self.target,
                self.model,
                self.compatibles,
            )

    def test_required_identity_properties_cannot_be_omitted(self) -> None:
        """Neither model nor compatible may be inferred from another artifact."""
        for missing, tree in (
            ("model", self.tree(model=None)),
            ("compatible", self.tree(compatible=None)),
        ):
            with (
                self.subTest(missing=missing),
                self.assertRaisesRegex(DeviceTreeError, rf"root lacks property {missing}"),
            ):
                verify_target_identity(tree, self.target, self.model, self.compatibles)


class RootBootargsTests(unittest.TestCase):
    """Verify external-root behavior from binary FDT properties."""

    root: ClassVar[dict[str, object]] = {
        "kind": "external",
        "filesystem": "ext4",
        "partuuid": "46504c58-02",
        "wait_seconds": 10,
    }

    def tree(self, bootargs: str) -> bytes:
        """Build one binary /chosen node with the requested command line."""
        return binary_tree([], [("chosen", [("bootargs", bootargs.encode() + b"\0")], [])])

    def test_exact_external_root_contract_is_accepted(self) -> None:
        """Allow unrelated diagnostics around the exact persistent-root options."""
        verify_root_bootargs(
            self.tree(
                "console=tty0 root=PARTUUID=46504c58-02 rootfstype=ext4 "
                "rootwait=10 rw init=/sbin/init panic=-1"
            ),
            self.root,
        )

    def test_conflicting_or_unbounded_root_options_are_rejected(self) -> None:
        """Reject command lines that can mount a different or unbounded root."""
        valid = "root=PARTUUID=46504c58-02 rootfstype=ext4 rootwait=10 rw init=/sbin/init"
        cases = (
            valid.replace("46504c58-02", "46504c59-02"),
            valid + " root=/dev/mmcblk0p2",
            valid.replace("rootwait=10", "rootwait"),
            valid.replace("rw", "ro"),
            valid + " rdinit=/init",
            valid.replace("init=/sbin/init", "init=/init"),
        )
        for bootargs in cases:
            with self.subTest(bootargs=bootargs), self.assertRaises(DeviceTreeError):
                verify_root_bootargs(self.tree(bootargs), self.root)


class ProfileLayoutDtbTests(unittest.TestCase):
    """Verify fixed microSD boot-memory ownership from binary FDT properties."""

    layout: ClassVar[dict[str, int]] = {
        "ram_base": 0x80000000,
        "ram_size": 0x04000000,
        "fdt_load": 0x83E00000,
        "fdt_size": 0x00010000,
        "fdt_pad": 0x00003000,
        "framebuffer": 0x83F00000,
        "framebuffer_size": 0x00100000,
    }
    linux_memory: ClassVar[dict[str, int]] = {"base": 0x80000000, "size": 0x03E00000}

    def test_boot_layout_accepts_a_display_with_separate_dma_memory(self) -> None:
        """Native display buffers do not change the boot or external-root contract."""
        tree = _binary_profile_layout_tree()
        verify_profile_dtb_layout(tree, self.layout, self.linux_memory)
        verify_root_bootargs(
            tree,
            {
                "kind": "external",
                "filesystem": "ext4",
                "partuuid": "46504c58-02",
                "wait_seconds": 10,
            },
        )

    def test_linux_can_start_after_a_coprocessor_reservation(self) -> None:
        """A board's exact Linux range can exclude the first two MiB of physical RAM."""
        tree = _binary_profile_layout_tree(memory_base=0x80200000, memory_size=0x03C00000)
        verify_profile_dtb_layout(tree, self.layout, {"base": 0x80200000, "size": 0x03C00000})
        with self.assertRaisesRegex(DeviceTreeError, "lacks node /memory@80000000"):
            verify_profile_dtb_layout(tree, self.layout, self.linux_memory)

    def test_linux_cannot_claim_the_fixed_fdt_arena(self) -> None:
        """Even a matching target declaration cannot overlap the loaded DTB."""
        tree = _binary_profile_layout_tree(memory_size=0x03F00000)
        with self.assertRaisesRegex(DeviceTreeError, "overlaps"):
            verify_profile_dtb_layout(tree, self.layout, {"base": 0x80000000, "size": 0x03F00000})

    def test_memory_reservation_and_padded_fdt_mismatches_are_rejected(self) -> None:
        """Reject changed RAM ownership or an oversized DTB before the U-Boot handoff."""
        memory = _binary_profile_layout_tree(memory_size=0x03DFF000)
        reserved = _binary_profile_layout_tree(reserved_range=(0x83F00000, 0x000FF000))
        mapped = _binary_profile_layout_tree(reserved_no_map=False)
        padded = _binary_profile_layout_tree()
        too_small = dict(self.layout)
        too_small["fdt_size"] = len(padded) + 0x3000 - 1
        cases = (
            (memory, self.layout, "memory range"),
            (reserved, self.layout, "framebuffer reservation"),
            (mapped, self.layout, "must be no-map"),
            (padded, too_small, "padding exceeds"),
        )
        for tree, layout, message in cases:
            with (
                self.subTest(message=message),
                self.assertRaisesRegex(DeviceTreeError, message),
            ):
                verify_profile_dtb_layout(tree, layout, self.linux_memory)


if __name__ == "__main__":
    unittest.main()

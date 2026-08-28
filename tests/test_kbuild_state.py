# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for the exact Kbuild success receipt."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli import builder, kbuild_state


class KbuildStateTests(unittest.TestCase):
    """Kbuild reuses only complete outputs for the same exact recipe."""

    def setUp(self) -> None:
        """Create isolated Kbuild inputs and output paths."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / "work"
        self.work.mkdir()
        self.output = self.work / "kernel"
        self.linux = self.root / "linux"
        self.defconfig = self._write("defconfig", b"CONFIG_TEST=y\n")
        self.rootfs = self._write("rootfs.cpio", b"rootfs-a\n")
        self.cross = "arm-none-eabi-"

    def _write(self, relative: str, contents: bytes, *, root: Path | None = None) -> Path:
        path = (self.root if root is None else root) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def _plan(self, jobs: int = 2, *, external: bool = False) -> kbuild_state.KbuildPlan:
        initramfs = None if external else kbuild_state.initramfs_identity(self.rootfs)
        initramfs_input = (
            None if initramfs is None else kbuild_state.initramfs_input_path(self.work, initramfs)
        )
        root: dict[str, object] = (
            {
                "kind": "external",
                "filesystem": "ext4",
                "partuuid": "46504c58-02",
                "wait_seconds": 10,
            }
            if external
            else {"kind": "initramfs"}
        )
        kbuild = [
            "make",
            "-C",
            str(self.linux),
            f"O={self.output}",
            "ARCH=arm",
            f"CROSS_COMPILE={self.cross}",
        ]
        return kbuild_state.create_plan(
            linux_recipe="a" * 64,
            linux_base="c" * 64,
            defconfig=self.defconfig,
            defconfig_path="targets/demo/kernel/defconfig",
            root=root,
            initramfs=initramfs,
            initramfs_input=initramfs_input,
            initramfs_receipt=(None if external else {"recipe": "d" * 64, "sha256": "e" * 64}),
            arch="arm",
            cross_compile=self.cross,
            commands=builder.kernel_build_commands(
                kbuild,
                [
                    "scripts/config",
                    "--file",
                    str(self.output / ".config"),
                    "--set-str",
                    "INITRAMFS_SOURCE",
                    "" if initramfs_input is None else str(initramfs_input),
                ],
                ["zImage", "dtbs"],
                jobs,
            ),
            outputs=("arch/zImage", "arch/demo.dtb", "vmlinux", "System.map", ".config"),
            implementation=[],
        )

    def _write_outputs(self, tag: bytes) -> None:
        self._write("arch/zImage", b"zImage-" + tag, root=self.output)
        self._write("arch/demo.dtb", b"dtb-" + tag, root=self.output)
        self._write("vmlinux", b"vmlinux-" + tag, root=self.output)
        self._write("System.map", b"map-" + tag, root=self.output)
        self._write(".config", b"CONFIG_TEST=y\n", root=self.output)

    def _complete(self, plan: kbuild_state.KbuildPlan, tag: bytes) -> None:
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.rootfs, plan)
        self._write_outputs(tag)
        kbuild_state.publish_success(self.work, self.output, plan)

    def test_exact_hit_keeps_fixed_output(self) -> None:
        """An exact receipt is reusable without disturbing retained Kbuild state."""
        plan = self._plan()
        self._complete(plan, b"a")
        retained = self._write("drivers/retained.o", b"keep\n", root=self.output)

        self.assertTrue(kbuild_state.cache_hit(self.work, self.output, self._plan()))
        self.assertEqual(retained.read_bytes(), b"keep\n")

    def test_parallelism_does_not_change_the_artifact_recipe(self) -> None:
        """Treat ``-j`` as scheduling while retaining the command shape."""
        self.assertEqual(self._plan(1).recipe, self._plan(8).recipe)

    def test_changed_input_is_a_miss_but_retains_fixed_output(self) -> None:
        """A new recipe leaves ``work/kernel`` for Kbuild's own reconciliation."""
        first = self._plan()
        self._complete(first, b"a")
        retained = self._write("drivers/retained.o", b"keep\n", root=self.output)
        self.defconfig.write_bytes(b"CONFIG_TEST=n\n")
        changed = self._plan()

        self.assertNotEqual(first.recipe, changed.recipe)
        self.assertFalse(kbuild_state.cache_hit(self.work, self.output, changed))
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.rootfs, changed)
        self.assertEqual(self.output, self.work / "kernel")
        self.assertEqual(retained.read_bytes(), b"keep\n")

    def test_outputs_without_success_receipt_are_a_miss(self) -> None:
        """Populated output paths alone cannot become a cache hit."""
        plan = self._plan()
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.rootfs, plan)
        self._write_outputs(b"partial")

        self.assertFalse((self.work / kbuild_state.RECEIPT_NAME).exists())
        self.assertFalse(kbuild_state.cache_hit(self.work, self.output, plan))

    def test_success_receipt_publishes_complete_outputs_as_a_hit(self) -> None:
        """A receipt published after all outputs exist authorizes reuse."""
        plan = self._plan()
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.rootfs, plan)
        self._write_outputs(b"complete")

        self.assertFalse(kbuild_state.cache_hit(self.work, self.output, plan))
        kbuild_state.publish_success(self.work, self.output, plan)
        self.assertTrue(kbuild_state.cache_hit(self.work, self.output, plan))
        identity = kbuild_state.receipt_identity(self.work, self.output, plan)
        self.assertEqual(identity["recipe"], plan.recipe)

    def test_changed_initramfs_input_revokes_hit_and_success_publication(self) -> None:
        """A receipt cannot reuse or republish outputs with another initramfs copy."""
        plan = self._plan()
        self._complete(plan, b"a")
        if plan.initramfs_input is None:
            self.fail("embedded plan did not expose its initramfs input")
        plan.initramfs_input.write_bytes(b"rootfs-tampered\n")

        self.assertFalse(kbuild_state.cache_hit(self.work, self.output, plan))
        with self.assertRaisesRegex(kbuild_state.KbuildStateError, "initramfs input"):
            kbuild_state.publish_success(self.work, self.output, plan)

    def test_external_root_has_no_materialized_initramfs_dependency(self) -> None:
        """External root reuse depends on boot parameters, not filesystem bytes."""
        plan = self._plan(external=True)
        kbuild_state.prepare_output(self.work, self.output)
        self._write_outputs(b"external")
        kbuild_state.publish_success(self.work, self.output, plan)

        self.rootfs.write_bytes(b"unrelated external filesystem bytes\n")

        self.assertTrue(kbuild_state.cache_hit(self.work, self.output, self._plan(external=True)))
        self.assertFalse((self.work / "rootfs.cpio").exists())
        with self.assertRaisesRegex(kbuild_state.KbuildStateError, "does not consume"):
            kbuild_state.materialize_initramfs_input(self.work, self.rootfs, plan)

    def test_changing_external_root_contract_is_a_kbuild_miss(self) -> None:
        """Compiled bootargs cannot reuse a receipt for another PARTUUID."""
        plan = self._plan(external=True)
        kbuild_state.prepare_output(self.work, self.output)
        self._write_outputs(b"external")
        kbuild_state.publish_success(self.work, self.output, plan)
        changed = kbuild_state.create_plan(
            linux_recipe="a" * 64,
            linux_base="c" * 64,
            defconfig=self.defconfig,
            defconfig_path="targets/demo/kernel/defconfig",
            root={**plan.root, "partuuid": "46504c59-02"},
            initramfs=None,
            initramfs_input=None,
            initramfs_receipt=None,
            arch="arm",
            cross_compile=self.cross,
            commands=[["make", "olddefconfig"]],
            outputs=("arch/zImage", "arch/demo.dtb", "vmlinux", "System.map", ".config"),
            implementation=[],
        )

        self.assertNotEqual(plan.recipe, changed.recipe)
        self.assertFalse(kbuild_state.cache_hit(self.work, self.output, changed))


if __name__ == "__main__":
    unittest.main()

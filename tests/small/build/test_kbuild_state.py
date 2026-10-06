# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for exact kernel-output cache state."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.kernel import compile as kernel_build
from fplinux_cli.build.kernel import receipts as kbuild_state

if TYPE_CHECKING:
    from pathlib import Path


class KernelOutputStateTests:
    """The kernel-output state component reuses only complete exact outputs."""

    @pytest.fixture(autouse=True)
    def _kernel_output(self, tmp_path: Path) -> None:
        """Create isolated kernel-output inputs and paths."""
        self.root = tmp_path
        self.work = self.root / "work"
        self.work.mkdir()
        self.output = self.work / "kernel"
        self.linux = self.root / "linux"
        self.defconfig = self._write("defconfig", b"CONFIG_TEST=y\n")
        self.initramfs = self._write("initramfs.cpio", b"boot archive a\n")
        self.cross = "arm-none-eabi-"

    def _write(self, relative: str, contents: bytes, *, root: Path | None = None) -> Path:
        path = (self.root if root is None else root) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def _plan(
        self,
        jobs: int = 2,
        *,
        external: bool = False,
        partuuid: str = "46504c58-02",
    ) -> kbuild_state.KbuildPlan:
        initramfs = None if external else kbuild_state.initramfs_identity(self.initramfs)
        initramfs_input = (
            None if initramfs is None else kbuild_state.initramfs_input_path(self.work, initramfs)
        )
        root: dict[str, object] = (
            {
                "kind": "external",
                "filesystem": "ext4",
                "partuuid": partuuid,
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
            commands=kernel_build.kernel_build_commands(
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
        kbuild_state.materialize_initramfs_input(self.work, self.initramfs, plan)
        self._write_outputs(tag)
        kbuild_state.publish_success(self.work, self.output, plan)

    def test_exact_hit_keeps_fixed_output(self) -> None:
        """An exact receipt is reusable without disturbing retained output state."""
        plan = self._plan()
        self._complete(plan, b"a")
        retained = self._write("drivers/retained.o", b"keep\n", root=self.output)

        assert kbuild_state.cache_hit(self.work, self.output, self._plan())
        assert (retained.read_bytes()) == (b"keep\n")

    def test_changed_boot_archive_invalidates_cache_and_updates_materialized_input(self) -> None:
        """Different boot bytes change the plan identity and its materialized input."""
        before = self._plan()
        self._complete(before, b"a")
        if before.initramfs_input is None:
            pytest.fail("RAM plan has no materialized boot input")
        assert (before.initramfs_input.read_bytes()) == (b"boot archive a\n")
        self.initramfs.write_bytes(b"boot archive b\n")
        changed = self._plan()
        assert (before.recipe) != (changed.recipe)
        assert not (kbuild_state.cache_hit(self.work, self.output, changed))
        kbuild_state.materialize_initramfs_input(self.work, self.initramfs, changed)
        if changed.initramfs_input is None:
            pytest.fail("changed RAM plan has no materialized boot input")
        assert (changed.initramfs_input.read_bytes()) == (b"boot archive b\n")

    def test_parallelism_is_not_a_recipe_input(self) -> None:
        """Scheduling changes do not select a different cache slot."""
        assert (self._plan(1).recipe) == (self._plan(8).recipe)

    def test_changed_input_is_a_miss_but_retains_fixed_output(self) -> None:
        """A new recipe leaves the fixed output path available for reconciliation."""
        first = self._plan()
        self._complete(first, b"a")
        retained = self._write("drivers/retained.o", b"keep\n", root=self.output)
        self.defconfig.write_bytes(b"CONFIG_TEST=n\n")
        changed = self._plan()

        assert (first.recipe) != (changed.recipe)
        assert not (kbuild_state.cache_hit(self.work, self.output, changed))
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.initramfs, changed)
        assert (self.output) == (self.work / "kernel")
        assert (retained.read_bytes()) == (b"keep\n")

    def test_outputs_without_success_receipt_are_a_miss(self) -> None:
        """Populated output paths alone cannot become a cache hit."""
        plan = self._plan()
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.initramfs, plan)
        self._write_outputs(b"partial")

        assert not ((self.work / kbuild_state.RECEIPT_NAME).exists())
        assert not (kbuild_state.cache_hit(self.work, self.output, plan))

    def test_success_receipt_publishes_complete_outputs_as_a_hit(self) -> None:
        """A receipt published after all outputs exist authorizes reuse."""
        plan = self._plan()
        kbuild_state.prepare_output(self.work, self.output)
        kbuild_state.materialize_initramfs_input(self.work, self.initramfs, plan)
        self._write_outputs(b"complete")

        assert not (kbuild_state.cache_hit(self.work, self.output, plan))
        kbuild_state.publish_success(self.work, self.output, plan)
        assert kbuild_state.cache_hit(self.work, self.output, plan)
        identity = kbuild_state.receipt_identity(self.work, self.output, plan)
        assert (identity["recipe"]) == (plan.recipe)

    def test_changed_initramfs_input_revokes_hit_and_success_publication(self) -> None:
        """A receipt cannot reuse or republish outputs with another initramfs copy."""
        plan = self._plan()
        self._complete(plan, b"a")
        if plan.initramfs_input is None:
            pytest.fail("embedded plan did not expose its initramfs input")
        plan.initramfs_input.write_bytes(b"rootfs-tampered\n")

        assert not (kbuild_state.cache_hit(self.work, self.output, plan))
        with pytest.raises(kbuild_state.KbuildStateError, match="initramfs input"):
            kbuild_state.publish_success(self.work, self.output, plan)

    def test_external_root_has_no_materialized_initramfs_dependency(self) -> None:
        """An external plan stays reusable after boot archive edits and rejects materialization."""
        plan = self._plan(external=True)
        kbuild_state.prepare_output(self.work, self.output)
        self._write_outputs(b"external")
        kbuild_state.publish_success(self.work, self.output, plan)

        self.initramfs.write_bytes(b"unrelated boot archive bytes\n")

        assert kbuild_state.cache_hit(self.work, self.output, self._plan(external=True))
        assert not ((self.work / "rootfs.cpio").exists())
        with pytest.raises(kbuild_state.KbuildStateError, match="does not consume"):
            kbuild_state.materialize_initramfs_input(self.work, self.initramfs, plan)

    def test_changing_external_root_contract_is_a_cache_miss(self) -> None:
        """One PARTUUID cannot authorize reuse for a different external root."""
        plan = self._plan(external=True)
        kbuild_state.prepare_output(self.work, self.output)
        self._write_outputs(b"external")
        kbuild_state.publish_success(self.work, self.output, plan)
        changed = self._plan(external=True, partuuid="46504c59-02")

        assert (plan.recipe) != (changed.recipe)
        assert not (kbuild_state.cache_hit(self.work, self.output, changed))

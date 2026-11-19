# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for exact kernel-output cache state."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.build.kernel import compile as kernel_build
from fplinux_cli.build.kernel import receipts as kbuild_state

if TYPE_CHECKING:
    from pathlib import Path


class KernelOutputContext:
    """The kernel-output state component reuses only complete exact outputs."""

    def __init__(self, root: Path) -> None:
        """Create isolated kernel-output inputs and paths."""
        self.root = root
        self.work = self.root / "work"
        self.work.mkdir()
        self.output = self.work / "kernel"
        self.linux = self.root / "linux"
        self.defconfig = self.write("defconfig", b"CONFIG_TEST=y\n")
        self.cross = "arm-none-eabi-"

    def write(self, relative: str, contents: bytes, *, root: Path | None = None) -> Path:
        """Write one input or output inside its explicitly selected temporary tree."""
        path = (self.root if root is None else root) / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def plan(self, jobs: int = 2) -> kbuild_state.KbuildPlan:
        """Construct a cache plan from the SoC configuration and compiler arguments."""
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
                    "",
                ],
                ["zImage", "dtbs"],
                jobs,
            ),
            outputs=("arch/zImage", "arch/demo.dtb", "vmlinux", "System.map", ".config"),
            implementation=[],
        )

    def write_outputs(self, tag: bytes) -> None:
        """Supply complete deterministic outputs at the kernel consumer boundary."""
        self.write("arch/zImage", b"zImage-" + tag, root=self.output)
        self.write("arch/demo.dtb", b"dtb-" + tag, root=self.output)
        self.write("vmlinux", b"vmlinux-" + tag, root=self.output)
        self.write("System.map", b"map-" + tag, root=self.output)
        self.write(".config", b"CONFIG_TEST=y\n", root=self.output)

    def complete(self, plan: kbuild_state.KbuildPlan, tag: bytes) -> None:
        """Publish a successful receipt after materializing all declared output files."""
        kbuild_state.prepare_output(self.work, self.output)
        self.write_outputs(tag)
        kbuild_state.publish_success(self.work, self.output, plan)


@pytest.fixture
def kernel_output(tmp_path: Path) -> KernelOutputContext:
    """Supply owned kernel inputs and outputs for one cache-state scenario."""
    return KernelOutputContext(tmp_path)


class KernelOutputStateTests:
    """The kernel-output component reuses only complete exact outputs."""

    def test_exact_hit_keeps_fixed_output(self, kernel_output: KernelOutputContext) -> None:
        """An exact receipt is reusable without disturbing retained output state."""
        plan = kernel_output.plan()
        kernel_output.complete(plan, b"a")
        retained = kernel_output.write("drivers/retained.o", b"keep\n", root=kernel_output.output)

        assert kbuild_state.cache_hit(
            kernel_output.work, kernel_output.output, kernel_output.plan()
        )
        assert (retained.read_bytes()) == (b"keep\n")

    def test_parallelism_is_not_a_recipe_input(self, kernel_output: KernelOutputContext) -> None:
        """Scheduling changes do not select a different cache slot."""
        assert (kernel_output.plan(1).recipe) == (kernel_output.plan(8).recipe)

    def test_changed_input_is_a_miss_but_retains_fixed_output(
        self, kernel_output: KernelOutputContext
    ) -> None:
        """A new recipe leaves the fixed output path available for reconciliation."""
        first = kernel_output.plan()
        kernel_output.complete(first, b"a")
        retained = kernel_output.write("drivers/retained.o", b"keep\n", root=kernel_output.output)
        kernel_output.defconfig.write_bytes(b"CONFIG_TEST=n\n")
        changed = kernel_output.plan()

        assert (first.recipe) != (changed.recipe)
        assert not (kbuild_state.cache_hit(kernel_output.work, kernel_output.output, changed))
        kbuild_state.prepare_output(kernel_output.work, kernel_output.output)
        assert (kernel_output.output) == (kernel_output.work / "kernel")
        assert (retained.read_bytes()) == (b"keep\n")

    def test_success_receipt_publishes_complete_outputs_as_a_hit(
        self, kernel_output: KernelOutputContext
    ) -> None:
        """A receipt published after all outputs exist authorizes reuse."""
        plan = kernel_output.plan()
        kbuild_state.prepare_output(kernel_output.work, kernel_output.output)
        kernel_output.write_outputs(b"complete")

        assert not (kernel_output.work / kbuild_state.RECEIPT_NAME).exists()
        assert not (kbuild_state.cache_hit(kernel_output.work, kernel_output.output, plan))
        kbuild_state.publish_success(kernel_output.work, kernel_output.output, plan)
        assert kbuild_state.cache_hit(kernel_output.work, kernel_output.output, plan)
        identity = kbuild_state.receipt_identity(kernel_output.work, kernel_output.output, plan)
        assert (identity["recipe"]) == (plan.recipe)

    def test_missing_binary_refuses_reuse_and_receipt_publication(
        self, kernel_output: KernelOutputContext
    ) -> None:
        """The receipt can only authorize a complete set of compiler outputs."""
        plan = kernel_output.plan()
        kernel_output.complete(plan, b"before")
        (kernel_output.output / "arch/zImage").unlink()

        assert not kbuild_state.cache_hit(kernel_output.work, kernel_output.output, plan)
        with pytest.raises(kbuild_state.KbuildStateError, match="declared output is missing"):
            kbuild_state.publish_success(kernel_output.work, kernel_output.output, plan)

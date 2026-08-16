# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for the content-addressed shared-toolchain contract."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli import toolchain_state


class ToolchainStateTests(unittest.TestCase):
    """Exercise the shared-toolchain recipe and receipt contract."""

    def setUp(self) -> None:
        """Create a platform toolchain declaration and one toolchain tree."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.platform = {
            "buildroot": {
                "external": "buildroot-external",
                "toolchain_defconfig": "platforms/demo/toolchain.defconfig",
                "toolchain_external_defconfig": "platforms/demo/toolchain-external.defconfig",
            },
            "linux": {"cross_compile": "arm-"},
        }
        self.container_lock = {"buildroot": {"version": "fake", "sha256": "1" * 64}}
        self.container_recipe = "2" * 64
        self._write("platforms/demo/toolchain.defconfig", "BR2_arm=y\n")
        self._write("platforms/demo/toolchain-external.defconfig", "BR2_TOOLCHAIN_EXTERNAL=y\n")

    def _write(self, relative: str, contents: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        return path

    def _recipe(self) -> str:
        return toolchain_state.toolchain_recipe(
            self.root,
            self.platform,
            self.container_lock,
            self.container_recipe,
        )

    def _toolchain(self) -> tuple[Path, tuple[str, ...]]:
        outputs = toolchain_state.toolchain_outputs(self.platform)
        toolchain = self.root / "toolchains" / self._recipe()
        self._write(f"toolchains/{self._recipe()}/host/bin/arm-gcc", "compiler\n")
        return toolchain, outputs

    def test_toolchain_defconfig_change_changes_recipe(self) -> None:
        """A toolchain configuration change must produce a new toolchain."""
        before = self._recipe()
        self._write("platforms/demo/toolchain.defconfig", "BR2_arm=y\nBR2_CCACHE=y\n")
        self.assertNotEqual(before, self._recipe())

    def test_patches_change_changes_recipe(self) -> None:
        """A Buildroot patch-tree change feeds the toolchain identity."""
        before = self._recipe()
        self._write("buildroot-external/patches/linux-headers/x.hash", "hash\n")
        self.assertNotEqual(before, self._recipe())

    def test_external_fragment_does_not_change_recipe(self) -> None:
        """The consumer fragment shapes rootfs recipes, not the toolchain."""
        before = self._recipe()
        self._write("platforms/demo/toolchain-external.defconfig", "BR2_TOOLCHAIN_EXTERNAL=n\n")
        self.assertEqual(before, self._recipe())

    def test_receipt_round_trip_is_a_hit(self) -> None:
        """A written receipt for this exact recipe and outputs matches."""
        toolchain, outputs = self._toolchain()
        toolchain_state.write_receipt(toolchain, self._recipe(), outputs)

        self.assertTrue(toolchain_state.receipt_matches(toolchain, self._recipe(), outputs))

    def test_garbage_receipt_is_a_miss(self) -> None:
        """An unreadable receipt requests a fresh toolchain build."""
        toolchain, outputs = self._toolchain()
        toolchain_state.write_receipt(toolchain, self._recipe(), outputs)
        (toolchain / toolchain_state.RECEIPT_NAME).write_text("GARBAGE", encoding="utf-8")

        self.assertFalse(toolchain_state.receipt_matches(toolchain, self._recipe(), outputs))

    def test_missing_compiler_is_a_miss(self) -> None:
        """A receipt without its compiler on disk cannot be reused."""
        toolchain, outputs = self._toolchain()
        toolchain_state.write_receipt(toolchain, self._recipe(), outputs)
        (toolchain / outputs[0]).unlink()

        self.assertFalse(toolchain_state.receipt_matches(toolchain, self._recipe(), outputs))


if __name__ == "__main__":
    unittest.main()

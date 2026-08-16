# SPDX-License-Identifier: GPL-2.0-only
"""Focused tests for Buildroot recipes and receipts."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli import buildroot_state


class BuildrootStateTests(unittest.TestCase):
    """Exercise the minimal Buildroot recipe and receipt contract."""

    def setUp(self) -> None:
        """Create a Buildroot external tree with one local package."""
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.target_config = {"buildroot": {"defconfig": "rootfs/defconfig"}}
        self.platform = {
            "buildroot": {
                "external": "buildroot-external",
                "toolchain_defconfig": "platforms/demo/toolchain.defconfig",
                "toolchain_external_defconfig": "platforms/demo/toolchain-external.defconfig",
            },
            "linux": {"cross_compile": "arm-"},
        }
        self._write("platforms/demo/toolchain.defconfig", "BR2_arm=y\n")
        self._write("platforms/demo/toolchain-external.defconfig", "BR2_TOOLCHAIN_EXTERNAL=y\n")
        self.container_lock = {"buildroot": {"version": "fake", "sha256": "1" * 64}}
        self.container_recipe = "2" * 64
        self._write("targets/demo/rootfs/defconfig", "BR2_PACKAGE_CPUCLOCK=y\n")
        self._write("buildroot-external/package/cpuclock/cpuclock.mk", "VERSION = 1\n")
        self._write("buildroot-external/package/cpuclock/payload", "cpuclock A\n")

    def tearDown(self) -> None:
        """Release the temporary Buildroot fixture."""
        self.temporary.cleanup()

    def _write(self, relative: str, contents: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
        return path

    def _recipe(self) -> buildroot_state.BuildrootRecipe:
        return buildroot_state.buildroot_recipe(
            self.root,
            "demo",
            self.target_config,
            self.platform,
            self.container_lock,
            self.container_recipe,
            "3" * 64,
        )

    def _output(self) -> tuple[Path, tuple[str, str]]:
        output = self.root / "out"
        outputs = buildroot_state.buildroot_output_paths(self.platform)
        self._write("out/images/rootfs.cpio", "rootfs\n")
        self._write("out/host/bin/arm-gcc", "compiler\n")
        return output, outputs

    def test_local_package_source_change_changes_recipe(self) -> None:
        """A local package source update must invalidate its Buildroot recipe."""
        before = self._recipe()
        self._write("buildroot-external/package/cpuclock/payload", "cpuclock B\n")
        self.assertNotEqual(before, self._recipe())

    def test_missing_receipt_is_a_miss_that_requests_clean(self) -> None:
        """An existing output without a receipt is rebuilt with ``make clean``."""
        output, outputs = self._output()
        self.assertFalse(buildroot_state.receipt_matches(output, self._recipe(), outputs))

    def test_mismatched_receipt_is_a_miss_that_requests_clean(self) -> None:
        """A receipt for another recipe is rebuilt with ``make clean``."""
        output, outputs = self._output()
        buildroot_state.write_receipt(output, self._recipe(), outputs)
        self._write("targets/demo/rootfs/defconfig", "BR2_PACKAGE_CPUCLOCK=y\nBR2_CCACHE=y\n")

        self.assertFalse(buildroot_state.receipt_matches(output, self._recipe(), outputs))

    def test_package_payload_change_reports_the_stale_package(self) -> None:
        """A package source change rebuilds only that package in place."""
        output, outputs = self._output()
        buildroot_state.write_receipt(output, self._recipe(), outputs)
        self._write("buildroot-external/package/cpuclock/payload", "cpuclock B\n")

        recipe = self._recipe()

        self.assertFalse(buildroot_state.receipt_matches(output, recipe, outputs))
        self.assertEqual(buildroot_state.stale_packages(output, recipe, outputs), ("cpuclock",))

    def test_defconfig_change_requests_a_full_clean(self) -> None:
        """A configuration change invalidates the shared base, not one package."""
        output, outputs = self._output()
        buildroot_state.write_receipt(output, self._recipe(), outputs)
        self._write("targets/demo/rootfs/defconfig", "BR2_PACKAGE_CPUCLOCK=y\nBR2_CCACHE=y\n")

        self.assertIsNone(buildroot_state.stale_packages(output, self._recipe(), outputs))

    def test_package_config_in_change_requests_a_full_clean(self) -> None:
        """A package Config.in change alters the dependency graph, so clean."""
        output, outputs = self._output()
        buildroot_state.write_receipt(output, self._recipe(), outputs)
        self._write("buildroot-external/package/cpuclock/Config.in", "config CPUCLOCK\n")

        self.assertIsNone(buildroot_state.stale_packages(output, self._recipe(), outputs))

    def test_added_package_requests_a_full_clean(self) -> None:
        """A new package directory cannot be rebuilt in place."""
        output, outputs = self._output()
        buildroot_state.write_receipt(output, self._recipe(), outputs)
        self._write("buildroot-external/package/newpkg/newpkg.mk", "VERSION = 1\n")

        self.assertIsNone(buildroot_state.stale_packages(output, self._recipe(), outputs))

    def test_exact_success_receipt_is_a_hit(self) -> None:
        """A receipt records exactly the current recipe and required outputs."""
        output, outputs = self._output()
        recipe = self._recipe()
        buildroot_state.write_receipt(output, recipe, outputs)

        self.assertTrue(buildroot_state.receipt_matches(output, recipe, outputs))

    def test_discard_success_receipt_is_idempotent(self) -> None:
        """A replacement build can revoke the prior success before it mutates output."""
        output, outputs = self._output()
        recipe = self._recipe()
        buildroot_state.write_receipt(output, recipe, outputs)

        buildroot_state.discard_success_receipt(output)
        buildroot_state.discard_success_receipt(output)

        self.assertFalse((output / buildroot_state.RECEIPT_NAME).exists())


if __name__ == "__main__":
    unittest.main()

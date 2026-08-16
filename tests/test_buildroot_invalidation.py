# SPDX-License-Identifier: GPL-2.0-only
"""Buildroot cache-miss regression for sticky local package staging."""

from __future__ import annotations

import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import builder, buildroot_state, toolchain_state


class _StickyLocalPackageBuildroot:
    """Model local package staging that changes only after ``make clean``."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.clean_calls = 0
        self.dirclean_calls = 0
        self.build_calls = 0
        self.interrupt_next = False

    def run(self, command: list[str], **_kwargs: object) -> None:
        """Run one small sticky Buildroot simulation."""
        if self.interrupt_next:
            self.interrupt_next = False
            raise KeyboardInterrupt
        output = Path(next(part.removeprefix("O=") for part in command if part.startswith("O=")))
        target = command[-1]
        if target == "clean":
            self.clean_calls += 1
            for relative in ("build", "images", "host"):
                shutil.rmtree(output / relative, ignore_errors=True)
        elif target.endswith("-dirclean"):
            self.dirclean_calls += 1
            staged = output / "build/staged" / target.removesuffix("-dirclean")
            staged.unlink(missing_ok=True)
        elif target == "defconfig":
            staged = output / "build/staged/cpuclock"
            if not staged.exists():
                staged.parent.mkdir(parents=True, exist_ok=True)
                staged.write_bytes(
                    (self.root / "buildroot-external/package/cpuclock/payload").read_bytes()
                )
        elif target.startswith("-j"):
            self.build_calls += 1
            staged = output / "build/staged/cpuclock"
            if not staged.exists():
                staged.parent.mkdir(parents=True, exist_ok=True)
                staged.write_bytes(
                    (self.root / "buildroot-external/package/cpuclock/payload").read_bytes()
                )
            rootfs = output / "images/rootfs.cpio"
            rootfs.parent.mkdir(parents=True, exist_ok=True)
            rootfs.write_bytes(staged.read_bytes())
            compiler = output / "host/bin/arm-gcc"
            compiler.parent.mkdir(parents=True, exist_ok=True)
            compiler.write_text("compiler\n", encoding="utf-8")
        else:
            raise AssertionError(f"unexpected fake Buildroot target: {target}")


class BuildrootInvalidationTests(unittest.TestCase):
    """Prove cache misses clean sticky Buildroot local-package state."""

    def setUp(self) -> None:
        """Create a source tree and its Buildroot simulator."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        self.output = Path(self.temporary.name) / "output"
        self.target_config = {"buildroot": {"defconfig": "rootfs/defconfig"}}
        self.platform = {
            "buildroot": {
                "external": "buildroot-external",
                "toolchain_defconfig": "platforms/demo/toolchain.defconfig",
                "toolchain_external_defconfig": "platforms/demo/toolchain-external.defconfig",
            },
            "linux": {"cross_compile": "arm-"},
        }
        self.container_lock = {"buildroot": {"version": "fake", "sha256": "1" * 64}}
        self._write("platforms/demo/toolchain.defconfig", "BR2_arm=y\n")
        self._write("platforms/demo/toolchain-external.defconfig", "BR2_TOOLCHAIN_EXTERNAL=y\n")
        self.toolchain = Path(self.temporary.name) / "toolchain"
        self._write("targets/demo/rootfs/defconfig", "BR2_PACKAGE_CPUCLOCK=y\n")
        self._write("buildroot-external/package/cpuclock/cpuclock.mk", "VERSION = 1\n")
        self._write("buildroot-external/package/cpuclock/payload", "cpuclock A\n")
        self.runner = _StickyLocalPackageBuildroot(self.root)

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
            "2" * 64,
            "3" * 64,
        )

    def _build(self) -> Path:
        with (
            mock.patch.object(builder, "ROOT", self.root),
            mock.patch.object(builder, "CACHE", self.root / "cache"),
            mock.patch.object(builder, "run", side_effect=self.runner.run),
            mock.patch.object(
                builder, "buildroot_recipe", side_effect=lambda *_args: self._recipe()
            ),
            mock.patch.object(toolchain_state, "toolchain_recipe", return_value="3" * 64),
            mock.patch.object(builder, "log_message"),
        ):
            rootfs, _cross = builder.build_rootfs(
                "demo",
                self.target_config,
                self.platform,
                self.container_lock,
                self.output,
                1,
                self.toolchain,
            )
        return rootfs

    def test_cpuclock_source_change_rebuilds_only_the_package(self) -> None:
        """A cpuclock source change rebuilds cpuclock in place, keeping the base."""
        first = self._build()
        self.assertEqual(first.read_bytes(), b"cpuclock A\n")
        before = self._recipe()
        self._write("buildroot-external/package/cpuclock/payload", "cpuclock B\n")
        self.assertNotEqual(before, self._recipe())

        second = self._build()

        self.assertEqual(second.read_bytes(), b"cpuclock B\n")
        self.assertEqual(self.runner.clean_calls, 0)
        self.assertEqual(self.runner.dirclean_calls, 1)

    def test_defconfig_change_causes_full_clean(self) -> None:
        """A configuration change rebuilds the whole tree with ``make clean``."""
        self._build()
        self._write("targets/demo/rootfs/defconfig", "BR2_PACKAGE_CPUCLOCK=y\nBR2_CCACHE=y\n")

        self._build()

        self.assertEqual(self.runner.clean_calls, 1)
        self.assertEqual(self.runner.dirclean_calls, 0)

    def test_missing_receipt_causes_clean(self) -> None:
        """An output without a receipt is not reused."""
        self._build()
        (self.output / buildroot_state.RECEIPT_NAME).unlink()

        self._build()

        self.assertEqual(self.runner.clean_calls, 1)

    def test_mismatched_receipt_causes_clean(self) -> None:
        """An output with another recipe's receipt is not reused."""
        self._build()
        receipt = self.output / buildroot_state.RECEIPT_NAME
        payload = json.loads(receipt.read_text(encoding="utf-8"))
        payload["recipe"] = "0" * 64
        receipt.write_text(json.dumps(payload), encoding="utf-8")

        self._build()

        self.assertEqual(self.runner.clean_calls, 1)

    def test_success_receipt_skips_make(self) -> None:
        """A matching receipt avoids every Buildroot Make target."""
        self._build()
        counts = (self.runner.clean_calls, self.runner.build_calls)

        self._build()

        self.assertEqual((self.runner.clean_calls, self.runner.build_calls), counts)

    def test_interrupted_new_recipe_cannot_reuse_prior_success(self) -> None:
        """Ctrl-C after a recipe miss must revoke the preceding success receipt."""
        self._build()
        recipe_a = self._recipe()
        self._write("buildroot-external/package/cpuclock/payload", "cpuclock B\n")
        self.assertNotEqual(recipe_a, self._recipe())

        self.runner.interrupt_next = True
        with self.assertRaises(KeyboardInterrupt):
            self._build()
        self.assertFalse((self.output / buildroot_state.RECEIPT_NAME).exists())

        self._write("buildroot-external/package/cpuclock/payload", "cpuclock A\n")
        self.assertEqual(recipe_a, self._recipe())
        self._build()

        self.assertEqual((self.runner.clean_calls, self.runner.build_calls), (1, 2))


if __name__ == "__main__":
    unittest.main()

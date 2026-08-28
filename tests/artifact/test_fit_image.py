# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for profile-owned native U-Boot FIT images."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

from fplinux_cli import fit_image
from fplinux_cli.common import sha256_file

from tests.process import run_process


class FitImageTests(unittest.TestCase):
    """Build and reuse a FIT with the quality image's pinned U-Boot tools."""

    def setUp(self) -> None:
        """Copy explicitly declared U-Boot tools and create FIT inputs."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        source_mkimage, source_dumpimage = self._installed_tools()
        self.mkimage = self._copy_declared_tool(source_mkimage)
        self.dumpimage = self._copy_declared_tool(source_dumpimage)
        self.zimage = self.root / "zImage"
        self.dtb = self.root / "linux.dtb"
        self.output = self.root / "output"
        self.zimage.write_bytes(b"FPLinux test zImage payload\n")
        self.dtb.write_bytes(b"FPLinux test DTB payload\n")
        self.spec: dict[str, Any] = {
            "kind": "sha256",
            "filename": "FPLINUX.ITB",
            "kernel_load": 0x82000000,
            "kernel_entry": 0x82000000,
            "fdt_load": 0x83E00000,
        }
        self.tools_receipt = {"recipe": "a" * 64, "sha256": "b" * 64}

    def _installed_tools(self) -> tuple[Path, Path]:
        """Resolve the two required FIT tools from the pinned quality image."""
        resolved: list[Path] = []
        for name in ("mkimage", "dumpimage"):
            location = shutil.which(name)
            if location is None:
                self.fail(f"quality image lacks required FIT tool: {name}")
            resolved.append(Path(location))
        tools = (resolved[0], resolved[1])
        for tool in tools:
            try:
                result = run_process(
                    [str(tool), "-V"],
                    name=f"{tool.name} version check",
                    timeout=30,
                )
            except OSError as error:
                self.fail(f"quality FIT tool cannot run: {tool}: {error}")
            if result.returncode != 0:
                self.fail(f"quality FIT tool self-test failed: {tool}: {result.stderr}")
        return tools

    def _copy_declared_tool(self, source: Path) -> Path:
        """Isolate a supplied binary from concurrent cache publication."""
        destination = self.root / source.name
        try:
            shutil.copyfile(source, destination)
            destination.chmod(0o755)
        except OSError as error:
            self.fail(f"cannot copy quality FIT tool {source}: {error}")
        return destination

    def plan(self) -> fit_image.FitPlan:
        """Describe the current fixture inputs through the production recipe."""
        return fit_image.create_plan(
            "nokia-ta1618",
            "Nokia 3210 4G (TA-1618)",
            self.spec,
            self.zimage,
            self.dtb,
            self.tools_receipt,
        )

    def build(
        self,
        plan: fit_image.FitPlan | None = None,
        *,
        mkimage: Path | None = None,
    ) -> Path:
        """Publish the current FIT through the declared U-Boot tools."""
        return fit_image.build(
            self.mkimage if mkimage is None else mkimage,
            self.dumpimage,
            self.zimage,
            self.dtb,
            self.output,
            self.plan() if plan is None else plan,
        )

    def test_builds_a_verified_fit_artifact(self) -> None:
        """Real mkimage and dumpimage produce the declared reusable FIT artifact."""
        plan = self.plan()
        fit = self.build(plan)

        self.assertEqual(fit, self.output / self.spec["filename"])
        self.assertGreater(
            fit.stat().st_size,
            self.zimage.stat().st_size + self.dtb.stat().st_size,
        )
        self.assertTrue(fit_image.cache_hit(self.output, plan))

    def test_changed_zimage_misses_then_rebuilds_the_fit(self) -> None:
        """One declared kernel input change revokes only this FIT cache entry."""
        first = self.plan()
        self.build(first)
        before = sha256_file(self.output / self.spec["filename"])

        self.zimage.write_bytes(b"FPLinux changed zImage payload\n")
        changed = self.plan()

        self.assertFalse(fit_image.cache_hit(self.output, changed))
        rebuilt = self.build(changed)
        self.assertTrue(fit_image.cache_hit(self.output, changed))
        self.assertNotEqual(before, sha256_file(rebuilt))

    def test_unrelated_file_keeps_a_complete_cache_hit(self) -> None:
        """A sibling outside the declared kernel and DTB inputs cannot rebuild FIT."""
        plan = self.plan()
        self.build(plan)
        receipt = (self.output / fit_image.RECEIPT_NAME).read_bytes()
        (self.root / "unrelated-host-note").write_text("not a FIT input\n", encoding="utf-8")

        self.assertEqual(
            self.build(plan, mkimage=self.root / "must-not-run"),
            self.output / self.spec["filename"],
        )
        self.assertEqual((self.output / fit_image.RECEIPT_NAME).read_bytes(), receipt)

    def test_missing_and_tampered_fit_are_rebuilt(self) -> None:
        """Absent or changed published bytes cannot remain reusable."""
        plan = self.plan()
        fit = self.build(plan)
        expected = fit.read_bytes()
        fit.unlink()
        self.assertFalse(fit_image.cache_hit(self.output, plan))

        self.assertEqual(self.build(plan).read_bytes(), expected)
        fit.write_bytes(b"tampered\n")
        self.assertFalse(fit_image.cache_hit(self.output, plan))

        self.assertEqual(self.build(plan).read_bytes(), expected)
        self.assertTrue(fit_image.cache_hit(self.output, plan))

    def test_tool_failure_preserves_prior_complete_fit(self) -> None:
        """A failed new build cannot replace the previous verified publication."""
        plan = self.plan()
        fit = self.build(plan)
        prior_fit = fit.read_bytes()
        prior_receipt = (self.output / fit_image.RECEIPT_NAME).read_bytes()
        failing = self.root / "failing-mkimage"
        failing.write_text("#!/bin/sh\nexit 19\n", encoding="utf-8")
        failing.chmod(0o755)
        self.zimage.write_bytes(b"FPLinux changed zImage payload\n")
        changed = self.plan()

        with self.assertRaises(subprocess.CalledProcessError) as failure:
            self.build(changed, mkimage=failing)

        self.assertEqual(failure.exception.returncode, 19)
        self.assertEqual(fit.read_bytes(), prior_fit)
        self.assertEqual((self.output / fit_image.RECEIPT_NAME).read_bytes(), prior_receipt)
        self.assertTrue(fit_image.cache_hit(self.output, plan))


if __name__ == "__main__":
    unittest.main()

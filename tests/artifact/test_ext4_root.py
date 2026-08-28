# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for profile-owned ext4 root filesystem images."""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

from fplinux_cli import ext4_root
from fplinux_cli.common import sha256_file


class Ext4RootTests(unittest.TestCase):
    """Build and inspect ext4 images through the real filesystem tools."""

    def setUp(self) -> None:
        """Create one small normalized root tree and ext4 profile."""
        required = ("mke2fs", "e2fsck", "debugfs")
        missing = [name for name in required if shutil.which(name) is None]
        if missing:
            self.skipTest("required ext4 tools are unavailable: " + ", ".join(missing))
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "normalized-root"
        (self.source / "etc").mkdir(parents=True)
        self.os_release = self.source / "etc/os-release"
        self.os_release.write_text(
            'NAME="FPLinux"\nID=fplinux\nVERSION_ID="test"\n', encoding="utf-8"
        )
        (self.source / "etc/issue").write_text("FPLinux test root\n", encoding="utf-8")
        self.output = self.root / "output"
        self.spec: dict[str, Any] = {
            "kind": "ext4-root",
            "filename": "FPLROOT.ext4",
            "partuuid": "46504c58-02",
            "label": "FPLROOT",
            "uuid": "042681b5-d000-5b78-9c16-8e8b2944594e",
            "size": 16 * 1024 * 1024,
            "block_size": 4096,
            "inode_size": 256,
        }

    def plan(self, rootfs_recipe: str = "a" * 64) -> ext4_root.Ext4Plan:
        """Create one valid declared identity for the unchanged root tree."""
        return ext4_root.create_plan(
            self.spec,
            rootfs_recipe,
            {"recipe": rootfs_recipe, "sha256": "b" * 64},
            "c" * 64,
        )

    def build(self, plan: ext4_root.Ext4Plan | None = None) -> Path:
        """Publish the current fixture tree as its profile-owned image."""
        if plan is None:
            plan = self.plan()
        return ext4_root.build(self.source, self.output, plan)

    def dump_os_release(self, image: Path) -> bytes:
        """Read one known file through ext4, independent of the producer receipt."""
        dumped = self.root / "dumped-os-release"
        result = subprocess.run(
            ["debugfs", "-R", f"dump /etc/os-release {dumped}", str(image)],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        return dumped.read_bytes()

    def test_builds_a_valid_ext4_artifact(self) -> None:
        """The producer publishes the requested ext4 filesystem and root content."""
        plan = self.plan()
        image = self.build(plan)

        self.assertEqual(image, self.output / "FPLROOT.ext4")
        self.assertEqual(image.stat().st_size, self.spec["size"])
        with image.open("rb") as stream:
            stream.seek(1024 + 56)
            self.assertEqual(struct.unpack("<H", stream.read(2))[0], 0xEF53)
        checked = subprocess.run(
            ["e2fsck", "-f", "-n", str(image)],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        self.assertEqual(checked.returncode, 0, checked.stderr or checked.stdout)
        self.assertEqual(self.dump_os_release(image), self.os_release.read_bytes())
        self.assertTrue(ext4_root.cache_hit(self.output, plan))

    def test_changed_rootfs_identity_misses_then_replaces_the_receipt(self) -> None:
        """One named rootfs identity change invalidates this ext4 cache entry."""
        first = self.plan()
        self.build(first)
        before = sha256_file(self.output / ext4_root.RECEIPT_NAME)

        changed = self.plan("d" * 64)

        self.assertFalse(ext4_root.cache_hit(self.output, changed))
        self.build(changed)
        self.assertTrue(ext4_root.cache_hit(self.output, changed))
        self.assertNotEqual(before, sha256_file(self.output / ext4_root.RECEIPT_NAME))
        self.assertEqual(
            self.dump_os_release(self.output / self.spec["filename"]),
            self.os_release.read_bytes(),
        )

    def test_unrelated_sibling_does_not_revoke_a_complete_cache_hit(self) -> None:
        """A file outside the declared normalized root tree has no causal effect."""
        plan = self.plan()
        self.build(plan)
        receipt = (self.output / ext4_root.RECEIPT_NAME).read_bytes()

        (self.root / "unrelated-host-note").write_text("not part of rootfs\n", encoding="utf-8")

        self.assertTrue(ext4_root.cache_hit(self.output, plan))
        self.assertEqual((self.output / ext4_root.RECEIPT_NAME).read_bytes(), receipt)

    def test_missing_and_tampered_images_are_rebuilt(self) -> None:
        """Absent or modified published bytes cannot remain reusable."""
        plan = self.plan()
        image = self.build(plan)
        image.unlink()
        self.assertFalse(ext4_root.cache_hit(self.output, plan))

        image = self.build(plan)
        image.write_bytes(b"tampered\n")
        self.assertFalse(ext4_root.cache_hit(self.output, plan))

        rebuilt = self.build(plan)
        self.assertTrue(ext4_root.cache_hit(self.output, plan))
        self.assertEqual(self.dump_os_release(rebuilt), self.os_release.read_bytes())

    def test_rejected_root_tree_preserves_previous_complete_artifact(self) -> None:
        """A validation failure before publication leaves the prior image reusable."""
        plan = self.plan()
        image = self.build(plan)
        prior_image = image.read_bytes()
        prior_receipt = (self.output / ext4_root.RECEIPT_NAME).read_bytes()
        try:
            os.setxattr(self.os_release, "user.fplinux-test", b"unsupported")
        except OSError as error:
            self.skipTest(f"filesystem cannot create a test xattr: {error}")

        with self.assertRaisesRegex(ext4_root.Ext4RootError, "xattrs"):
            self.build(plan)

        self.assertEqual(image.read_bytes(), prior_image)
        self.assertEqual((self.output / ext4_root.RECEIPT_NAME).read_bytes(), prior_receipt)
        self.assertTrue(ext4_root.cache_hit(self.output, plan))


if __name__ == "__main__":
    unittest.main()

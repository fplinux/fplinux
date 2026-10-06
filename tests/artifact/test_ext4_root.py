# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for profile-owned ext4 root filesystem images."""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.build.storage import ext4 as ext4_root
from fplinux_cli.common import sha256_file

if TYPE_CHECKING:
    from pathlib import Path


class Ext4RootTests:
    """Build and inspect ext4 images through the real filesystem tools."""

    @pytest.fixture(autouse=True)
    def _prepare_inputs(self, tmp_path: Path) -> None:
        """Create one small normalized root tree and ext4 profile."""
        required = ("mke2fs", "e2fsck", "debugfs")
        missing = [name for name in required if shutil.which(name) is None]
        if missing:
            pytest.fail("quality image lacks required ext4 tools: " + ", ".join(missing))
        self.root = tmp_path
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
        assert (result.returncode) == (0), result.stderr or result.stdout
        return dumped.read_bytes()

    def test_builds_a_valid_ext4_artifact(self) -> None:
        """The producer publishes the requested ext4 filesystem and root content."""
        plan = self.plan()
        image = self.build(plan)

        assert (image) == (self.output / "FPLROOT.ext4")
        assert (image.stat().st_size) == (self.spec["size"])
        with image.open("rb") as stream:
            stream.seek(1024 + 56)
            assert (struct.unpack("<H", stream.read(2))[0]) == (0xEF53)
        checked = subprocess.run(
            ["e2fsck", "-f", "-n", str(image)],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        assert (checked.returncode) == (0), checked.stderr or checked.stdout
        assert (self.dump_os_release(image)) == (self.os_release.read_bytes())
        assert ext4_root.cache_hit(self.output, plan)

    def test_changed_rootfs_identity_misses_then_replaces_the_receipt(self) -> None:
        """One named rootfs identity change invalidates this ext4 cache entry."""
        first = self.plan()
        self.build(first)
        before = sha256_file(self.output / ext4_root.RECEIPT_NAME)

        changed = self.plan("d" * 64)

        assert not (ext4_root.cache_hit(self.output, changed))
        self.build(changed)
        assert ext4_root.cache_hit(self.output, changed)
        assert (before) != (sha256_file(self.output / ext4_root.RECEIPT_NAME))
        assert (self.dump_os_release(self.output / self.spec["filename"])) == (
            self.os_release.read_bytes()
        )

    def test_missing_and_tampered_images_are_rebuilt(self) -> None:
        """Absent or modified published bytes cannot remain reusable."""
        plan = self.plan()
        image = self.build(plan)
        image.unlink()
        assert not (ext4_root.cache_hit(self.output, plan))

        image = self.build(plan)
        image.write_bytes(b"tampered\n")
        assert not (ext4_root.cache_hit(self.output, plan))

        rebuilt = self.build(plan)
        assert ext4_root.cache_hit(self.output, plan)
        assert (self.dump_os_release(rebuilt)) == (self.os_release.read_bytes())

    def test_rejected_root_tree_preserves_previous_complete_artifact(self) -> None:
        """A validation failure before publication leaves the prior image reusable."""
        plan = self.plan()
        image = self.build(plan)
        prior_image = image.read_bytes()
        prior_receipt = (self.output / ext4_root.RECEIPT_NAME).read_bytes()
        try:
            os.setxattr(self.os_release, "user.fplinux-test", b"unsupported")
        except OSError as error:
            pytest.skip(f"filesystem cannot create a test xattr: {error}")

        with pytest.raises(ext4_root.Ext4RootError, match="xattrs"):
            self.build(plan)

        assert (image.read_bytes()) == (prior_image)
        assert ((self.output / ext4_root.RECEIPT_NAME).read_bytes()) == (prior_receipt)
        assert ext4_root.cache_hit(self.output, plan)

# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem contracts for restoring installed image owners and permissions."""

from __future__ import annotations

import json
import os
import shutil
import stat
import tempfile
import unittest
from pathlib import Path

from fplinux_cli.environment.image_content import (
    image_content_digest,
    image_content_records,
    restore_image_metadata,
    validate_image_metadata,
)


def installed_tree(root: Path) -> Path:
    """Create a set-id tool, owned directories, a link and runtime state."""
    compiler = root / "usr/bin/cc"
    compiler.parent.mkdir(parents=True)
    (root / "usr").chmod(0o751)
    compiler.parent.chmod(0o2750)
    compiler.write_bytes(b"compiler\n")
    compiler.chmod(0o4755)
    (compiler.parent / "compiler").symlink_to("cc")
    state = root / "etc/fplinux-image-state"
    state.parent.mkdir()
    state.parent.chmod(0o755)
    state.write_bytes(b"runtime state\n")
    return compiler


class ImageMetadataTests(unittest.TestCase):
    """Captured ownership and modes apply only to the original installed content."""

    def test_capture_describes_actual_files_links_and_permissions(self) -> None:
        """The manifest carries owners, set-id modes and actual content identity."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            installed_tree(root)
            owner = {"uid": os.getuid(), "gid": os.getgid()}
            expected = [
                {"path": "etc", "mode": 0o755, "type": "directory", **owner},
                {"path": "usr", "mode": 0o751, "type": "directory", **owner},
                {"path": "usr/bin", "mode": 0o2750, "type": "directory", **owner},
                {
                    "path": "usr/bin/cc",
                    "mode": 0o4755,
                    "type": "file",
                    "sha256": "850d9371d0ce2cb083d4075d7eec49f2f6f3c2bee5b58d0226b36028ae84efe0",
                    **owner,
                },
                {
                    "path": "usr/bin/compiler",
                    "mode": 0o777,
                    "type": "symlink",
                    "target": "cc",
                    **owner,
                },
            ]
            self.assertEqual(image_content_records(root), expected)

    def test_restore_recovers_set_id_modes_and_original_identity(self) -> None:
        """Transport-lost permissions are restored after chown clears set-id bits."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = installed_tree(root)
            original = image_content_digest(root)
            metadata = json.loads(json.dumps(image_content_records(root)))
            compiler.chmod(0o755)
            compiler.parent.chmod(0o755)
            (root / "usr").chmod(0o755)
            (root / "etc/fplinux-image-state").write_bytes(b"other runtime state\n")
            self.assertNotEqual(image_content_digest(root), original)
            restore_image_metadata(root, list(reversed(metadata)))
            self.assertEqual(stat.S_IMODE(compiler.stat().st_mode), 0o4755)
            self.assertEqual(stat.S_IMODE(compiler.parent.stat().st_mode), 0o2750)
            self.assertEqual(stat.S_IMODE((root / "usr").stat().st_mode), 0o751)
            self.assertEqual(image_content_digest(root), original)

    def test_content_mismatch_leaves_all_current_permissions_unchanged(self) -> None:
        """No metadata is applied when bytes, types, links or the inventory differ."""
        for difference in ("bytes", "type", "link", "extra", "missing"):
            with self.subTest(difference=difference), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                compiler = installed_tree(root)
                metadata = image_content_records(root)
                compiler.chmod(0o755)
                compiler.parent.chmod(0o755)
                if difference == "bytes":
                    compiler.write_bytes(b"different compiler\n")
                elif difference == "type":
                    compiler.unlink()
                    compiler.mkdir()
                elif difference == "link":
                    link = compiler.parent / "compiler"
                    link.unlink()
                    link.symlink_to("other")
                elif difference == "extra":
                    (compiler.parent / "extra").write_bytes(b"unrecorded\n")
                else:
                    compiler.unlink()
                before = image_content_records(root)
                with self.assertRaisesRegex(ValueError, "content does not match"):
                    restore_image_metadata(root, metadata)
                self.assertEqual(image_content_records(root), before)

    def test_restore_accepts_an_unchanged_external_symlink(self) -> None:
        """An unchanged external link keeps its target outside the metadata operation."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root = base / "image"
            root.mkdir()
            outside = base / "outside"
            outside.write_bytes(b"outside\n")
            outside.chmod(0o600)
            (root / "bin").symlink_to(outside)
            restore_image_metadata(root, image_content_records(root))
            self.assertTrue((root / "bin").is_symlink())
            self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o600)

    def test_restore_rejects_symlink_parent_before_changing_any_metadata(self) -> None:
        """A parent replaced by a link cannot direct metadata writes outside the root."""
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root = base / "image"
            root.mkdir()
            compiler = installed_tree(root)
            metadata = image_content_records(root)
            outside = base / "outside"
            outside.mkdir()
            outside.chmod(0o700)
            external_compiler = outside / "cc"
            external_compiler.write_bytes(b"compiler\n")
            external_compiler.chmod(0o600)
            shutil.rmtree(compiler.parent)
            compiler.parent.symlink_to(outside)
            with self.assertRaisesRegex(ValueError, "parent is not a directory"):
                restore_image_metadata(root, metadata)
            self.assertEqual(stat.S_IMODE(outside.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(external_compiler.stat().st_mode), 0o600)

    def test_invalid_manifest_is_rejected_before_changing_permissions(self) -> None:
        """Invalid paths, owners, modes and entry shapes fail before metadata writes."""
        invalid_fields: tuple[tuple[str, object], ...] = (
            ("path", "../outside"),
            ("path", "."),
            ("path", "/usr/bin/cc"),
            ("path", "usr//bin/cc"),
            ("mode", 0o10000),
            ("mode", True),
            ("uid", -1),
            ("gid", "0"),
            ("type", "fifo"),
            ("sha256", "not a digest"),
        )
        for key, value in invalid_fields:
            with self.subTest(key=key, value=value), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                compiler = installed_tree(root)
                metadata = image_content_records(root)
                next(record for record in metadata if record["path"] == "usr/bin/cc")[key] = value
                compiler.chmod(0o755)
                with self.assertRaises(ValueError):
                    restore_image_metadata(root, metadata)
                self.assertEqual(stat.S_IMODE(compiler.stat().st_mode), 0o755)

    def test_manifest_validator_rejects_duplicate_paths_and_unknown_fields(self) -> None:
        """The host reader can reject ambiguous metadata without an installed image."""
        directory: dict[str, object] = {
            "path": "usr",
            "mode": 0o755,
            "uid": 0,
            "gid": 0,
            "type": "directory",
        }
        invalid_records: tuple[object, ...] = (
            [directory, directory],
            [{**directory, "mtime": 1}],
            {"entries": []},
        )
        for records in invalid_records:
            with self.subTest(records=records), self.assertRaises(ValueError):
                validate_image_metadata(records)
        self.assertEqual(validate_image_metadata([directory]), [directory])

    def test_restore_recovers_distinct_owners_in_mapped_namespace(self) -> None:
        """Actual filesystem owner changes preserve files, links and the content hash."""
        if os.geteuid() != 0:
            self.skipTest("requires root in a user namespace with uid/gid 1001 and 1002 mapped")
        for mapping in ("uid_map", "gid_map"):
            mapping_lines = Path(f"/proc/self/{mapping}").read_text(encoding="ascii").splitlines()
            ranges = [line.split() for line in mapping_lines]
            if not any(int(start) <= 1002 < int(start) + int(count) for start, _, count in ranges):
                self.skipTest("requires uid/gid 1001 and 1002 mapped")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = installed_tree(root)
            link = compiler.parent / "compiler"
            os.chown(compiler, 1001, 1002)
            compiler.chmod(0o4755)
            os.chown(link, 1002, 1001, follow_symlinks=False)
            original = image_content_digest(root)
            metadata = image_content_records(root)
            os.chown(compiler, 0, 0)
            os.chown(link, 0, 0, follow_symlinks=False)
            compiler.chmod(0o755)
            restore_image_metadata(root, metadata)
            self.assertEqual((compiler.stat().st_uid, compiler.stat().st_gid), (1001, 1002))
            self.assertEqual((link.lstat().st_uid, link.lstat().st_gid), (1002, 1001))
            self.assertEqual(stat.S_IMODE(compiler.stat().st_mode), 0o4755)
            self.assertEqual(image_content_digest(root), original)


if __name__ == "__main__":
    unittest.main()

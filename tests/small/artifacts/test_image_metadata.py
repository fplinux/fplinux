# SPDX-License-Identifier: GPL-2.0-only
"""Host filesystem contracts for restoring installed image owners and permissions."""

from __future__ import annotations

import json
import os
import shutil
import stat
from pathlib import Path

import pytest
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


class ImageMetadataTests:
    """Captured ownership and modes apply only to the original installed content."""

    @staticmethod
    def test_capture_describes_actual_files_links_and_permissions(tmp_path: Path) -> None:
        """The manifest carries owners, set-id modes and actual content identity."""
        root = tmp_path
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
        assert (image_content_records(root)) == (expected)

    @staticmethod
    def test_restore_recovers_set_id_modes_and_original_identity(tmp_path: Path) -> None:
        """Transport-lost permissions are restored after chown clears set-id bits."""
        root = tmp_path
        compiler = installed_tree(root)
        original = image_content_digest(root)
        metadata = json.loads(json.dumps(image_content_records(root)))
        compiler.chmod(0o755)
        compiler.parent.chmod(0o755)
        (root / "usr").chmod(0o755)
        (root / "etc/fplinux-image-state").write_bytes(b"other runtime state\n")
        assert (image_content_digest(root)) != (original)
        restore_image_metadata(root, list(reversed(metadata)))
        assert (stat.S_IMODE(compiler.stat().st_mode)) == (0o4755)
        assert (stat.S_IMODE(compiler.parent.stat().st_mode)) == (0o2750)
        assert (stat.S_IMODE((root / "usr").stat().st_mode)) == (0o751)
        assert (image_content_digest(root)) == (original)

    @staticmethod
    @pytest.mark.parametrize("difference", ["bytes", "type", "link", "extra", "missing"])
    def test_content_mismatch_leaves_all_current_permissions_unchanged(
        tmp_path: Path, difference: str
    ) -> None:
        """No metadata is applied when bytes, types, links or the inventory differ."""
        root = tmp_path
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
        with pytest.raises(ValueError, match="content does not match"):
            restore_image_metadata(root, metadata)
        assert (image_content_records(root)) == (before)

    @staticmethod
    def test_restore_accepts_an_unchanged_external_symlink(tmp_path: Path) -> None:
        """An unchanged external link keeps its target outside the metadata operation."""
        base = tmp_path
        root = base / "image"
        root.mkdir()
        outside = base / "outside"
        outside.write_bytes(b"outside\n")
        outside.chmod(0o600)
        (root / "bin").symlink_to(outside)
        restore_image_metadata(root, image_content_records(root))
        assert (root / "bin").is_symlink()
        assert (stat.S_IMODE(outside.stat().st_mode)) == (0o600)

    @staticmethod
    def test_restore_rejects_symlink_parent_before_changing_any_metadata(tmp_path: Path) -> None:
        """A parent replaced by a link cannot direct metadata writes outside the root."""
        base = tmp_path
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
        with pytest.raises(ValueError, match="parent is not a directory"):
            restore_image_metadata(root, metadata)
        assert (stat.S_IMODE(outside.stat().st_mode)) == (0o700)
        assert (stat.S_IMODE(external_compiler.stat().st_mode)) == (0o600)

    @staticmethod
    @pytest.mark.parametrize(
        ("key", "value"),
        [
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
        ],
        ids=[
            "parent-path",
            "root-path",
            "absolute-path",
            "empty-path-component",
            "mode-out-of-range",
            "boolean-mode",
            "negative-owner",
            "string-group",
            "unsupported-type",
            "invalid-digest",
        ],
    )
    def test_invalid_manifest_is_rejected_before_changing_permissions(
        tmp_path: Path, key: str, value: object
    ) -> None:
        """Invalid paths, owners, modes and entry shapes fail before metadata writes."""
        root = tmp_path
        compiler = installed_tree(root)
        metadata = image_content_records(root)
        next(record for record in metadata if record["path"] == "usr/bin/cc")[key] = value
        compiler.chmod(0o755)
        # Rejection type is the contract; invalid fields have different messages.
        with pytest.raises(ValueError):  # noqa: PT011
            restore_image_metadata(root, metadata)
        assert (stat.S_IMODE(compiler.stat().st_mode)) == (0o755)

    @staticmethod
    @pytest.mark.parametrize("invalid_shape", ["duplicate", "unknown-field", "not-a-list"])
    def test_manifest_validator_rejects_duplicate_paths_and_unknown_fields(
        invalid_shape: str,
    ) -> None:
        """The host reader can reject ambiguous metadata without an installed image."""
        directory: dict[str, object] = {
            "path": "usr",
            "mode": 0o755,
            "uid": 0,
            "gid": 0,
            "type": "directory",
        }
        invalid_records: dict[str, object] = {
            "duplicate": [directory, directory],
            "unknown-field": [{**directory, "mtime": 1}],
            "not-a-list": {"entries": []},
        }
        # Rejection type is the contract; invalid shapes have different messages.
        with pytest.raises(ValueError):  # noqa: PT011
            validate_image_metadata(invalid_records[invalid_shape])
        assert (validate_image_metadata([directory])) == ([directory])

    @staticmethod
    def test_restore_recovers_distinct_owners_in_mapped_namespace(tmp_path: Path) -> None:
        """Actual filesystem owner changes preserve files, links and the content hash."""
        if os.geteuid() != 0:
            pytest.skip("requires root in a user namespace with uid/gid 1001 and 1002 mapped")
        for mapping in ("uid_map", "gid_map"):
            mapping_lines = Path(f"/proc/self/{mapping}").read_text(encoding="ascii").splitlines()
            ranges = [line.split() for line in mapping_lines]
            if not any(int(start) <= 1002 < int(start) + int(count) for start, _, count in ranges):
                pytest.skip("requires uid/gid 1001 and 1002 mapped")
        root = tmp_path
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
        assert ((compiler.stat().st_uid, compiler.stat().st_gid)) == ((1001, 1002))
        assert ((link.lstat().st_uid, link.lstat().st_gid)) == ((1002, 1001))
        assert (stat.S_IMODE(compiler.stat().st_mode)) == (0o4755)
        assert (image_content_digest(root)) == (original)

# SPDX-License-Identifier: GPL-2.0-only
"""Identify and preserve installed contents consumed by the build environment."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, NotRequired, TypedDict

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence


class ImageMetadataRecord(TypedDict):
    """Installed entry metadata and the content it may be applied to."""

    path: str
    mode: int
    uid: int
    gid: int
    type: str
    sha256: NotRequired[str]
    target: NotRequired[str]


def image_content_records(root: Path) -> list[dict[str, object]]:
    """Collect installed bytes, paths, owners, modes and links without timestamps."""
    records: list[dict[str, object]] = []
    # Runtime mounts and scratch directories do not supply installed build tools.
    for directory in ("bin", "etc", "home", "lib", "opt", "root", "sbin", "usr", "var"):
        base = root / directory
        if not base.exists():
            continue
        paths = [base, *sorted(base.rglob("*"))] if not base.is_symlink() else [base]
        for path in paths:
            relative = path.relative_to(root).as_posix()
            # Kern supplies these per invocation; they are not installed tool inputs.
            if relative in {
                "etc/fplinux-image-state",
                "etc/hostname",
                "etc/hosts",
                "etc/resolv.conf",
            }:
                continue
            metadata = path.lstat()
            record: dict[str, object] = {
                "path": relative,
                "mode": stat.S_IMODE(metadata.st_mode),
                "uid": metadata.st_uid,
                "gid": metadata.st_gid,
            }
            if stat.S_ISLNK(metadata.st_mode):
                record["type"] = "symlink"
                record["target"] = path.readlink().as_posix()
            elif stat.S_ISDIR(metadata.st_mode):
                record["type"] = "directory"
            elif stat.S_ISREG(metadata.st_mode):
                record["type"] = "file"
                with path.open("rb") as source:
                    record["sha256"] = hashlib.file_digest(source, "sha256").hexdigest()
            else:
                raise ValueError(f"unsupported installed image entry: {relative}")
            records.append(record)
    return records


def image_content_digest(root: Path) -> str:
    """Hash installed bytes, paths, owners, modes and links without filesystem timestamps."""
    records = image_content_records(root)
    encoded = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _metadata_number(record: Mapping[str, object], key: str) -> int:
    value = record.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"invalid installed image metadata {key}")
    return value


def validate_image_metadata(records: object) -> list[ImageMetadataRecord]:
    """Validate an installed metadata manifest without reading or changing an image."""
    if not isinstance(records, list):
        message = "installed image metadata must be a list"
        raise ValueError(message)  # noqa: TRY004 - malformed JSON uses one validation error type.
    result: list[ImageMetadataRecord] = []
    paths: set[str] = set()
    for record in records:
        if not isinstance(record, dict):
            message = "installed image metadata entry must be an object"
            raise ValueError(message)  # noqa: TRY004 - malformed JSON uses one validation error type.
        relative = record.get("path")
        if not isinstance(relative, str):
            message = "invalid installed image metadata path"
            raise ValueError(message)  # noqa: TRY004 - malformed JSON uses one validation error type.
        path = PurePosixPath(relative)
        if (
            not relative
            or not path.parts
            or "\x00" in relative
            or path.is_absolute()
            or path.as_posix() != relative
            or any(part in {".", ".."} for part in path.parts)
            or relative in paths
        ):
            raise ValueError(f"invalid installed image metadata path: {relative}")
        entry_type = record.get("type")
        if not isinstance(entry_type, str) or entry_type not in {"directory", "file", "symlink"}:
            raise ValueError(f"invalid installed image metadata type: {relative}")
        mode = _metadata_number(record, "mode")
        if mode > 0o7777:
            raise ValueError(f"invalid installed image metadata mode: {relative}")
        validated: ImageMetadataRecord = {
            "path": relative,
            "mode": mode,
            "uid": _metadata_number(record, "uid"),
            "gid": _metadata_number(record, "gid"),
            "type": entry_type,
        }
        keys = {"path", "mode", "uid", "gid", "type"}
        if entry_type == "file":
            digest = record.get("sha256")
            if (
                not isinstance(digest, str)
                or len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)
            ):
                raise ValueError(f"invalid installed image metadata digest: {relative}")
            validated["sha256"] = digest
            keys.add("sha256")
        elif entry_type == "symlink":
            target = record.get("target")
            if not isinstance(target, str) or not target or "\x00" in target:
                raise ValueError(f"invalid installed image metadata link: {relative}")
            validated["target"] = target
            keys.add("target")
        if record.keys() != keys:
            raise ValueError(f"invalid installed image metadata fields: {relative}")
        paths.add(relative)
        result.append(validated)
    return result


def _content_identity(records: Sequence[Mapping[str, object]]) -> dict[str, dict[str, object]]:
    return {
        str(record["path"]): {
            key: value for key, value in record.items() if key not in {"mode", "uid", "gid"}
        }
        for record in records
    }


def restore_image_metadata(root: Path, records: object) -> None:
    """Restore owners and modes only after the complete installed content matches."""
    metadata = validate_image_metadata(records)
    root = root.resolve(strict=True)
    for record in metadata:
        parent = root
        for part in PurePosixPath(record["path"]).parts[:-1]:
            parent /= part
            if not stat.S_ISDIR(parent.lstat().st_mode):
                raise ValueError(f"installed image metadata parent is not a directory: {parent}")
    current = image_content_records(root)
    if _content_identity(current) != _content_identity(metadata):
        message = "installed image content does not match its metadata"
        raise ValueError(message)
    current_by_path = {str(record["path"]): record for record in current}
    changed_owners: set[str] = set()
    # Changing ownership can clear set-id bits; restore modes after every chown.
    for record in metadata:
        present = current_by_path[record["path"]]
        if (present["uid"], present["gid"]) != (record["uid"], record["gid"]):
            os.chown(root / record["path"], record["uid"], record["gid"], follow_symlinks=False)
            changed_owners.add(record["path"])
    # Restore children before a directory can lose its traversal permission.
    for record in sorted(metadata, key=lambda entry: entry["path"].count("/"), reverse=True):
        if record["type"] != "symlink":
            present = current_by_path[record["path"]]
            if present["mode"] != record["mode"] or record["path"] in changed_owners:
                (root / record["path"]).chmod(record["mode"], follow_symlinks=False)


def main() -> None:
    """Print installed identity, capture metadata or restore captured metadata."""
    parser = argparse.ArgumentParser(description=__doc__)
    operation = parser.add_mutually_exclusive_group()
    operation.add_argument(
        "--metadata", action="store_true", help="print installed image metadata"
    )
    operation.add_argument(
        "--restore-metadata", type=Path, help="restore installed image metadata"
    )
    args = parser.parse_args()
    if args.restore_metadata is not None:
        metadata = json.loads(args.restore_metadata.read_text(encoding="utf-8"))
        restore_image_metadata(Path("/"), metadata)
    elif args.metadata:
        print(json.dumps(image_content_records(Path("/")), sort_keys=True, separators=(",", ":")))
    else:
        print(image_content_digest(Path("/")))


if __name__ == "__main__":
    main()

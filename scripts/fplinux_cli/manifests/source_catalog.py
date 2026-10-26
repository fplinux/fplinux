# SPDX-License-Identifier: GPL-2.0-only
"""Resolve the project's direct originals and their single cache location."""

from __future__ import annotations

import re
from dataclasses import asdict, dataclass
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail, load_toml
from fplinux_cli.environment.downloads import download_locked_file
from fplinux_cli.manifests.values import basename_value, nonempty_string, relative_value

if TYPE_CHECKING:
    from pathlib import Path


@dataclass(frozen=True)
class ResolvedSource:
    """One pinned original, shared by acquisition and every local consumer."""

    id: str
    version: str
    name: str
    url: str
    algorithm: str
    checksum: str
    size: int | None
    destination: str
    archive_prefix: str | None = None

    def acquire(self, cache: Path, *, offline: bool = True) -> Path:
        """Acquire or verify the canonical original with its owner's checksum."""
        return download_locked_file(
            self.url,
            self.checksum,
            cache / self.destination,
            offline=offline,
            algorithm=self.algorithm,
            size=self.size,
        )


def resolve_source(root: Path, source_id: str) -> ResolvedSource:
    """Validate one selected direct source without admitting unrelated records."""
    return resolve_source_record(source_id, load_toml(root / "sources.lock.toml").get(source_id))


def resolve_source_record(source_id: str, record: object) -> ResolvedSource:
    """Resolve an already captured catalog record with the same exact contract."""
    if re.fullmatch(r"[a-z0-9][a-z0-9_-]*", source_id) is None:
        fail(f"invalid source identifier: {source_id}")
    if not isinstance(record, dict):
        fail(f"source lock entry is missing: {source_id}")
    url = nonempty_string(record.get("url"), f"source {source_id} URL")
    if not url.startswith("https://"):
        fail(f"source {source_id} URL must use HTTPS")
    algorithm = nonempty_string(record.get("algorithm"), f"source {source_id} algorithm")
    checksum = record.get("checksum")
    length = {"sha256": 64, "sha512": 128}.get(algorithm)
    if (
        length is None
        or not isinstance(checksum, str)
        or re.fullmatch(rf"[0-9a-f]{{{length}}}", checksum) is None
    ):
        fail(f"source {source_id} must have an exact SHA-256 or SHA-512 checksum")
    size = record.get("bytes")
    if size is not None and (type(size) is not int or size <= 0):
        fail(f"source {source_id} size must be a positive integer")
    version = nonempty_string(record.get("version"), f"source {source_id} version")
    name = basename_value(record.get("name"), f"source {source_id} name")
    prefix = record.get("archive_prefix")
    if prefix is not None:
        prefix = relative_value(prefix, f"source {source_id} archive_prefix")
    return ResolvedSource(
        source_id,
        version,
        name,
        url,
        algorithm,
        checksum,
        size,
        f"downloads/sources/{source_id}/{name}",
        prefix,
    )


def source_build_metadata(source_id: str, record: object) -> dict[str, Any]:
    """Project resolved fields used by native source builders and file projections."""
    if not isinstance(record, dict):
        fail(f"source lock entry is missing: {source_id}")
    source = resolve_source_record(source_id, record)
    metadata = {**record, **asdict(source), "source_id": source.id}
    if source.algorithm == "sha256":
        metadata.update(
            sha256=source.checksum, archive_url=source.url, archive_sha256=source.checksum
        )
    if re.fullmatch(r"[0-9a-f]{40}", source.version):
        metadata["commit"] = source.version
    return metadata


def acquire_source(root: Path, source_id: str, cache: Path, *, offline: bool = True) -> Path:
    """Resolve and acquire one original through the canonical owner."""
    return resolve_source(root, source_id).acquire(cache, offline=offline)


def load_host_sources(root: Path, lock_name: str) -> dict[str, dict[str, Any]]:
    """Resolve host use metadata while retaining its native package version and notices."""
    result = {}
    allowed = {"source", "directory"}
    if lock_name == "linux-host.lock.toml":
        allowed = {"source", "version", "licenses", "license_headers"}
    for name, use in load_toml(root / lock_name).items():
        if not isinstance(use, dict) or not isinstance(use.get("source"), str):
            fail(f"host source reference is missing: {lock_name}: {name}")
        if use.keys() - allowed:
            fail(f"host source use contains original metadata: {lock_name}: {name}")
        source = resolve_source(root, use["source"])
        metadata = asdict(source)
        metadata.pop("id")
        metadata["source_id"] = source.id
        metadata["sha256"] = source.checksum if source.algorithm == "sha256" else None
        metadata["size"] = source.size
        result[name] = {
            **metadata,
            **{key: value for key, value in use.items() if key != "source"},
        }
    return result

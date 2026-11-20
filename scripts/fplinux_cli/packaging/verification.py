# SPDX-License-Identifier: GPL-2.0-only
"""Verify the contents and associations of published release archives."""

from __future__ import annotations

import hashlib
import json
import re
import stat
import zipfile
from dataclasses import dataclass
from pathlib import PurePosixPath
from typing import TYPE_CHECKING, Any, cast

from fplinux_cli.artifacts.release import (
    RELEASE_VERSION_NAME,
    public_source_digest,
    source_release_version,
)
from fplinux_cli.artifacts.release_identity import RELEASE_FIELDS, release_context_from_fields
from fplinux_cli.common import canonical_json_bytes, display_text, fail, sha256_bytes, sha256_file
from fplinux_cli.dependencies.distribution import read_dependency_distribution
from fplinux_cli.packaging.card_kit import CARD_MEMBERS
from fplinux_cli.packaging.universal import release_archive_name
from fplinux_cli.runtime.bundle import MICROSD_BOOT_PROFILE
from fplinux_cli.runtime.release_catalog import parse_catalog, read_bundle_manifest
from fplinux_cli.workspace.capture import WorkspaceFile

from .distribution import distribution_records, require_public_runtime_files, source_distribution

if TYPE_CHECKING:
    from collections.abc import Collection
    from pathlib import Path

    from fplinux_cli.artifacts.release_identity import ReleaseContext


@dataclass(frozen=True)
class SourceArchive:
    """Verified source identity, runtime pairings and members."""

    release: ReleaseContext
    runtimes: list[dict[str, str]]
    files: dict[str, tuple[int, str]]


@dataclass(frozen=True)
class RuntimeArchive:
    """Verified runtime identity, target selections and members."""

    release: ReleaseContext
    targets: tuple[str, ...]
    files: dict[str, tuple[int, str]]


def _archive_checksums(archive: zipfile.ZipFile) -> tuple[str, dict[str, str]]:
    """Read the checksum list emitted by the FPLinux packager."""
    lists = [
        item for item in archive.infolist() if PurePosixPath(item.filename).name == "SHA256SUMS"
    ]
    if len(lists) != 1:
        fail("archive must contain one SHA256SUMS file")
    prefix = lists[0].filename.removesuffix("SHA256SUMS")
    checksums: dict[str, str] = {}
    for line in archive.read(lists[0]).decode("utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if match is None:
            fail("archive has an invalid SHA256SUMS entry")
        digest, name = match.groups()
        if name in checksums:
            fail(f"archive has a duplicate checksum entry: {name}")
        checksums[name] = digest
    return prefix, checksums


def _verify_archive(
    archive: zipfile.ZipFile,
) -> tuple[str, dict[str, str], dict[str, zipfile.ZipInfo]]:
    """Check the exact regular-file inventory and stream each checksum once."""
    prefix, checksums = _archive_checksums(archive)
    entries = [item for item in archive.infolist() if not item.is_dir()]
    files = {item.filename: item for item in entries}
    expected = {prefix + name for name in checksums} | {prefix + "SHA256SUMS"}
    if len(files) != len(entries) or set(files) != expected:
        fail("archive files do not match its SHA256SUMS inventory")
    for name, expected_digest in checksums.items():
        with archive.open(files[prefix + name]) as stream:
            digest = hashlib.file_digest(cast("zipfile.ZipExtFile", stream), "sha256").hexdigest()
        if digest != expected_digest:
            fail(f"archive checksum mismatch: {name}")
    return prefix, checksums, files


def _verify_source_archive(
    archive: zipfile.ZipFile, prefix: str, checksums: dict[str, str]
) -> tuple[ReleaseContext, list[dict[str, str]]]:
    data = archive.read(prefix + "DISTRIBUTION.json")
    records = distribution_records(data)
    context = release_context_from_fields(source_distribution(data))
    required = {
        "project/LICENSE",
        "project/THIRD_PARTY_NOTICES.md",
        "project/fplinux",
        f"project/{RELEASE_VERSION_NAME}",
    }
    members = {item.filename.removeprefix(prefix) for item in archive.infolist()}
    if not required <= members or not any(name.startswith("upstream/") for name in members):
        fail("corresponding sources lack project sources, notices or upstream material")
    for item in archive.infolist():
        if item.external_attr >> 16 not in {stat.S_IFREG | 0o644, stat.S_IFREG | 0o755}:
            fail(f"corresponding source has an invalid file mode: {item.filename}")
    retained = json.loads(archive.read(prefix + "project/.fplinux-release.json"))
    sources = tuple(
        WorkspaceFile(
            item.filename.removeprefix(prefix + "project/"),
            archive.read(item),
            stat.S_IMODE(item.external_attr >> 16),
        )
        for item in archive.infolist()
        if item.filename.startswith(prefix + "project/")
        and item.filename != prefix + "project/.fplinux-release.json"
    )
    if (
        not isinstance(retained, dict)
        or set(retained) != RELEASE_FIELDS
        or release_context_from_fields(retained) != context
        or public_source_digest(sources) != context.source_snapshot
        or source_release_version(sources) != context.release_version
    ):
        fail("corresponding project sources differ from their release identity")
    for record in records:
        _verify_source_material(archive, prefix, members, checksums, record)
    return context, records


def _verify_source_material(
    archive: zipfile.ZipFile,
    prefix: str,
    members: set[str],
    checksums: dict[str, str],
    record: dict[str, str],
) -> None:
    """Admit emitted rebuild instructions and every selected original they reference."""
    selection = f"selections/{record['target']}/{record['host']}/"
    required = {
        f"project/targets/{record['target']}/target.toml",
        selection + "upstream/downloads.json",
        selection + "upstream/alpine-source-channels.json",
    }
    if record["host"] == "linux":
        required.add("BUILD-LINUX.txt")
    else:
        required.update(
            {
                "upstream/windows-runtime-source-channels.json",
                selection + "sources/windows-tools-source.zip",
                selection + "sources/windows-driver/README.txt",
                selection + "sources/windows-driver/build.py",
                selection + "sources/windows-driver/downloads.json",
            }
        )
    missing = required - members
    if missing:
        fail(f"corresponding sources lack required build material: {min(missing)}")
    downloads = json.loads(archive.read(prefix + selection + "upstream/downloads.json"))
    if not isinstance(downloads, list) or not downloads:
        fail("corresponding sources have no selected upstream originals")
    for entry in downloads:
        if not isinstance(entry, dict) or entry.get("archive") not in checksums:
            fail("corresponding source inventory references a missing original")
        name = entry["archive"]
        algorithm = entry.get("algorithm")
        if algorithm == "sha256":
            digest = checksums[name]
        elif algorithm == "sha512":
            with archive.open(prefix + name) as stream:
                digest = hashlib.file_digest(
                    cast("zipfile.ZipExtFile", stream), "sha512"
                ).hexdigest()
        else:
            fail("corresponding source inventory has an unsupported checksum")
        if digest != entry.get("checksum"):
            fail(f"corresponding source original differs from its declaration: {name}")


def _archive_payload(archive: zipfile.ZipFile, prefix: str, parts: tuple[str, ...]) -> bytes:
    return b"".join(archive.read(prefix + part) for part in parts)


def _universal_manifests(
    archive: zipfile.ZipFile, prefix: str, checksums: dict[str, str]
) -> tuple[ReleaseContext, dict[str, dict[str, Any]]]:
    """Verify each reconstructed RAM and microSD runtime against its build manifest."""
    catalog = parse_catalog(archive.read(prefix + "targets.json"))
    require_public_runtime_files(checksums, archive.filename or "release")
    manifests = {}
    used_objects = set()
    for target, board in catalog.targets.items():
        card_members = set(CARD_MEMBERS) & board.files.keys()
        if (
            card_members != (set(CARD_MEMBERS) if board.microsd is not None else set())
            or (board.microsd is None and any(name.startswith("card/") for name in board.files))
        ):
            fail(f"release board has incomplete card installation files: {target}")
        for recipe, profile in ((board, None), (board.microsd, MICROSD_BOOT_PROFILE)):
            if recipe is None:
                continue
            require_public_runtime_files(recipe.files, target)
            build_manifest = "build-manifest.json"
            manifest = read_bundle_manifest(
                _archive_payload(archive, prefix, recipe.files[build_manifest]),
                build_manifest,
                target=target,
                release=catalog.release,
                profile=profile,
            )
            if manifest.get("build_type") != "release":
                fail(f"release board manifest has a different identity: {target}")
            runtime_manifest = "runtime-manifest.json"
            read_bundle_manifest(
                _archive_payload(archive, prefix, recipe.files[runtime_manifest]),
                runtime_manifest,
                target=target,
                release=catalog.release,
                profile=profile,
            )
            records = manifest.get("files")
            if not isinstance(records, dict) or set(records) != set(recipe.files) - {
                "build-manifest.json",
                "SHA256SUMS",
            }:
                fail(f"release board manifest has an incomplete file inventory: {target}")
            content = {key: value for key, value in manifest.items() if key != "generation"}
            if manifest.get("generation") != sha256_bytes(canonical_json_bytes(content)):
                fail(f"release board manifest generation differs: {target}")
            for name, parts in recipe.files.items():
                for part in parts:
                    if part.startswith("objects/"):
                        if checksums[part] != part.removeprefix("objects/"):
                            fail(f"release object differs from its name: {part}")
                        used_objects.add(part)
                if name not in records:
                    continue
                size = sum(archive.getinfo(prefix + part).file_size for part in parts)
                digest = (
                    checksums[parts[0]]
                    if len(parts) == 1
                    else sha256_bytes(_archive_payload(archive, prefix, parts))
                )
                mode = 0o755 if name in recipe.executables else 0o644
                if records[name] != {"mode": mode, "size": size, "sha256": digest}:
                    fail(f"release board file differs from its build manifest: {target}/{name}")
                if (
                    len(parts) == 1
                    and not parts[0].startswith("objects/")
                    and archive.getinfo(prefix + parts[0]).external_attr >> 16
                    != (stat.S_IFREG | mode)
                ):
                    fail(f"release shared file has the wrong executable mode: {parts[0]}")
            if profile is None:
                manifests[target] = manifest
            elif any(name.startswith("card/") for name in recipe.files):
                fail(f"installed card runtime includes installation files: {target}")
    if used_objects != {name for name in checksums if name.startswith("objects/")}:
        fail("release contains unreferenced objects")
    return catalog.release, manifests


def verify_archive(path: Path) -> SourceArchive | RuntimeArchive:
    """Check a release ZIP against its complete internal SHA256SUMS list."""
    try:
        with zipfile.ZipFile(path) as archive:
            prefix, checksums, members = _verify_archive(archive)
            files = {
                name: (members[prefix + name].file_size, digest)
                for name, digest in checksums.items()
            }
            if prefix + "DISTRIBUTION.json" in members:
                release, records = _verify_source_archive(archive, prefix, checksums)
                return SourceArchive(release, records, files)
            if prefix + "targets.json" in members:
                release, manifests = _universal_manifests(archive, prefix, checksums)
                return RuntimeArchive(release, tuple(sorted(manifests)), files)
            fail("archive has no FPLinux release catalog or corresponding source inventory")
    except (zipfile.BadZipFile, KeyError, ValueError) as error:
        fail(f"cannot inspect archive {display_text(path)}: {error}")


def verify_distribution(
    directory: Path,
    *,
    expected_matrix: Collection[tuple[str, str]] | None = None,
    require_inputs: bool = False,
) -> dict[str, Any]:
    """Check paired assets, optionally requiring the publisher's target and host selections."""
    try:
        metadata = json.loads((directory / "RELEASE.json").read_bytes())
        if not isinstance(metadata, dict) or set(metadata) != RELEASE_FIELDS | {
            "runtimes",
            "sources",
            "dependencies",
        }:
            fail("distribution has invalid release association metadata")
        release = release_context_from_fields(metadata)
        dependency_files: tuple[str, ...] = ()
        if metadata["dependencies"] == "DEPENDENCIES.json":
            dependency = read_dependency_distribution(directory)
            if dependency.release != release:
                fail("distribution dependency inputs belong to another release")
            dependency_files = tuple(dependency.files)
        elif require_inputs or metadata["dependencies"] is not None:
            fail("distribution has no valid dependency transport")
        source_name = f"FPLinux-{release.release_id}-sources.zip"
        if metadata["sources"] != {
            "archive": source_name,
            "sha256": sha256_file(directory / source_name),
        }:
            fail("distribution source archive differs from its release association")
        source = directory / source_name
        with zipfile.ZipFile(source) as archive:
            prefix, checksums, _files = _verify_archive(archive)
            source_release, records = _verify_source_archive(archive, prefix, checksums)
        if source_release != release or metadata["runtimes"] != records:
            fail("distribution source/runtime association differs")
        if expected_matrix is not None:
            actual = {(record["target"], record["host"]) for record in records}
            if actual != set(expected_matrix):
                fail("distribution does not contain the complete supported target/host matrix")
        expected_names = {
            source.name,
            "SHA256SUMS",
            "RELEASE.json",
            *dependency_files,
            *(record["archive"] for record in records),
        }
        if {path.name for path in directory.iterdir()} != expected_names:
            fail("distribution files do not match its runtime/source inventory")
        if any(path.is_symlink() or not path.is_file() for path in directory.iterdir()):
            fail("distribution requires regular files")
        outer = "".join(
            f"{sha256_file(directory / name)}  {name}\n"
            for name in sorted(expected_names - {"SHA256SUMS"})
        )
        if (directory / "SHA256SUMS").read_text(encoding="ascii") != outer:
            fail("distribution SHA256SUMS does not match its final assets")
        for host in sorted({record["host"] for record in records}):
            expected = release_archive_name(host, release.release_id)
            selected = {record["target"]: record for record in records if record["host"] == host}
            runtime = directory / expected
            if any(record["archive"] != expected for record in selected.values()):
                fail(f"distribution runtime name or pairing hash differs: {expected}")
            digest = sha256_file(runtime)
            if any(record["sha256"] != digest for record in selected.values()):
                fail(f"distribution runtime name or pairing hash differs: {expected}")
            with zipfile.ZipFile(runtime) as archive:
                prefix, checksums, _files = _verify_archive(archive)
                runtime_release, manifests = _universal_manifests(archive, prefix, checksums)
                if runtime_release != release or set(manifests) != set(selected):
                    fail(f"distribution runtime selection differs: {runtime.name}")
                for target, manifest in manifests.items():
                    if manifest.get("device_identity") != selected[target]["identity"]:
                        fail(f"distribution runtime identity differs: {runtime.name}/{target}")
                readme = archive.read(prefix + "README.txt").decode("utf-8")
                if f"\n  {source.name}\n" not in readme:
                    fail(f"distribution runtime names another source companion: {runtime.name}")
        for name in expected_names - {"SHA256SUMS"}:
            if (directory / name).stat().st_size >= 2 * 1024**3:
                fail(f"distribution asset must be smaller than 2 GiB: {name}")
    except (OSError, zipfile.BadZipFile, KeyError, ValueError, AttributeError) as error:
        fail(f"cannot inspect distribution {display_text(directory)}: {error}")
    return metadata

# SPDX-License-Identifier: GPL-2.0-only
"""Inspect published artifacts without building, extracting or executing them."""

from __future__ import annotations

import hashlib
import json
import re
import stat
import tarfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, cast

from fplinux_cli.artifacts.footprint import FootprintError, compare_footprints, inspect_footprint
from fplinux_cli.artifacts.release import release_context_from_fields
from fplinux_cli.common import canonical_json_bytes, display_text, fail, sha256_bytes, sha256_file
from fplinux_cli.runtime.bundle_session import resolve_target_bundle

if TYPE_CHECKING:
    from collections.abc import Mapping


def _print_identity(manifest: Mapping[str, object]) -> None:
    """Show the identity recorded in the inspected build, not the current source checkout."""
    release = release_context_from_fields(manifest)
    print(f"release_id: {release.release_id}")
    for key in ("target", "profile", "build_type", "generation"):
        value = manifest.get(key)
        if key == "profile" and value is None:
            value = "default"
        if not isinstance(value, str) or not value:
            fail(f"build manifest has no valid {key}")
        print(f"{key}: {value}")


def inspect_bundle(
    target: str, *, profile: str | None = None, build_type: str = "release"
) -> None:
    """Read the selected current generation and compare its files to its build manifest."""
    bundle, manifest = resolve_target_bundle(target, profile, build_type=build_type)
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        fail("build manifest has no file records")
    print(f"bundle: {display_text(bundle.path)}")
    _print_identity(manifest)
    print("SIZE SHA256 PATH")
    for relative, record in sorted(files.items()):
        artifact = bundle.path / relative
        size = artifact.stat().st_size
        digest = sha256_file(artifact)
        if (
            not isinstance(record, dict)
            or record.get("sha256") != digest
            or record.get("size") != size
        ):
            fail(f"bundle file differs from its build manifest: {relative}")
        print(f"{size} {digest} {relative}")
    print(f"files: {len(files)}; build-manifest checksums: OK")


def _byte_size(value: int) -> str:
    """Keep exact byte counts alongside a compact binary size for larger artifacts."""
    if abs(value) < 1024:
        return f"{value} B"
    return f"{value:,} B ({value / 1024:.1f} KiB)"


def inspect_target_footprint(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    json_output: bool = False,
) -> None:
    """Measure verified artifacts from the exact selected current bundle."""
    bundle, _manifest = resolve_target_bundle(target, profile, build_type=build_type)
    try:
        report = inspect_footprint(bundle)
    except FootprintError as error:
        fail(str(error))
    if json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
        return
    _print_identity(report["identity"])
    layers = report["layers"]
    print(layers["description"])
    print("Boot artifacts:")
    for name, size in sorted(layers["boot_artifact_bytes"].items()):
        print(f"  {_byte_size(size)}  {name}")
    print(f"Kernel {layers['kernel_image_name']}: {_byte_size(layers['kernel_image_bytes'])}")
    embedded = layers["embedded_initramfs"]
    if embedded is not None:
        print(
            f"Embedded initramfs ({embedded['compression']}): "
            f"{_byte_size(embedded['compressed_bytes'])}"
        )
    external_initramfs = layers["external_initramfs"]
    if external_initramfs is not None:
        print(f"External initramfs: {_byte_size(external_initramfs['cpio_bytes'])}")
    rootfs = report["rootfs"]
    print(f"Root filesystem ({rootfs['source']}):")
    print(f"  cpio: {_byte_size(rootfs['cpio_bytes'])}")
    print(f"  regular payload: {_byte_size(rootfs['regular_payload_bytes'])}")
    print(f"  symlink payload: {_byte_size(rootfs['symlink_payload_bytes'])}")
    print("Packages (owned regular and symlink payload; dependencies excluded):")
    for name, package in sorted(rootfs["packages"].items()):
        size = package["regular_payload_bytes"] + package["symlink_payload_bytes"]
        print(f"  {_byte_size(size)}  {name} {package['version']}")
    print("Optional APK archives:")
    for name, package in sorted(report["optional_apks"].items()):
        print(f"  {_byte_size(package['archive_bytes'])}  {name}")
    print("Host debug files:")
    for name, size in sorted(layers["host_debug_file_bytes"].items()):
        print(f"  {_byte_size(size)}  {name}")


def inspect_footprint_diff(before: Path, after: Path, *, json_output: bool = False) -> None:
    """Compare saved measurements without resolving a bundle or touching the cache."""
    try:
        old = json.loads(before.read_text(encoding="utf-8"))
        new = json.loads(after.read_text(encoding="utf-8"))
        if not isinstance(old, dict) or not isinstance(new, dict):
            fail("footprint reports must be JSON objects")
        report = compare_footprints(old, new)
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        fail(
            f"cannot compare footprint reports {display_text(before)} and "
            f"{display_text(after)}: {error}"
        )
    if json_output:
        print(json.dumps(report, indent=2, sort_keys=True))
        return
    print(f"before: {display_text(before)}")
    print(f"after: {display_text(after)}")
    print(f"same rootfs content: {'yes' if report['same_rootfs_content'] else 'no'}")
    print(
        f"Kernel {report['kernel_image_name']} delta: "
        f"{_byte_size(report['kernel_image_byte_delta'])}"
    )
    print("Boot artifact deltas:")
    for name, delta in report["boot_artifact_byte_delta"].items():
        print(f"  {_byte_size(delta)}  {name}")
    print("Root filesystem deltas:")
    for name, delta in report["rootfs_byte_delta"].items():
        print(f"  {_byte_size(delta)}  {name}")
    for kind in ("packages", "files", "optional_apks"):
        print(f"{kind}:")
        for change, entries in report[kind].items():
            print(f"  {change}: {', '.join(entries) or 'none'}")


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


def _source_records(
    archive: zipfile.ZipFile, prefix: str, checksums: dict[str, str]
) -> list[dict[str, str]]:
    from fplinux_cli.artifacts.release import (  # noqa: PLC0415 -- canonical source identity.
        public_source_digest,
    )
    from fplinux_cli.packaging.distribution import (  # noqa: PLC0415 -- source-only backend.
        distribution_records,
        source_distribution,
    )
    from fplinux_cli.workspace.capture import WorkspaceFile  # noqa: PLC0415 -- source records.

    data = archive.read(prefix + "DISTRIBUTION.json")
    records = distribution_records(data)
    context = release_context_from_fields(source_distribution(data))
    required = {"project/LICENSE", "project/THIRD_PARTY_NOTICES.md", "project/fplinux"}
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
        or release_context_from_fields(retained) != context
        or public_source_digest(sources) != context.source_snapshot
    ):
        fail("corresponding project sources differ from their release identity")
    for record in records:
        _verify_source_material(
            archive, prefix, members, checksums, record, combined=len(records) > 1
        )
    return records


def _verify_source_material(  # noqa: PLR0913 -- archive identity and selection inputs stay explicit.
    archive: zipfile.ZipFile,
    prefix: str,
    members: set[str],
    checksums: dict[str, str],
    record: dict[str, str],
    *,
    combined: bool,
) -> None:
    """Admit emitted rebuild instructions and every selected original they reference."""
    selection = f"selections/{record['target']}/{record['host']}/" if combined else ""
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


def _verify_runtime_manifest(
    manifest: dict[str, object],
    prefix: str,
    checksums: dict[str, str],
    files: dict[str, zipfile.ZipInfo],
) -> None:
    """Compare final manifest records to verified bytes and actual ZIP modes."""
    records = manifest.get("files")
    expected = set(checksums) - {"build-manifest.json"}
    if not isinstance(records, dict) or set(records) != expected:
        fail("distribution runtime build manifest has an incomplete file inventory")
    for name in expected:
        item = files[prefix + name]
        mode = item.external_attr >> 16
        if mode not in {stat.S_IFREG | 0o644, stat.S_IFREG | 0o755} or records[name] != {
            "mode": stat.S_IMODE(mode),
            "size": item.file_size,
            "sha256": checksums[name],
        }:
            fail(f"distribution runtime build manifest differs from final file: {name}")
    content = {key: value for key, value in manifest.items() if key != "generation"}
    if manifest.get("generation") != sha256_bytes(canonical_json_bytes(content)):
        fail("distribution runtime build manifest generation differs")


def inspect_archive(path: Path) -> None:
    """Check a release ZIP against its complete internal SHA256SUMS list."""
    try:
        with zipfile.ZipFile(path) as archive:
            prefix, checksums, files = _verify_archive(archive)
            print(f"archive: {display_text(path)}")
            if prefix + "DISTRIBUTION.json" in files:
                records = _source_records(archive, prefix, checksums)
                print("kind: corresponding-sources")
                for record in records:
                    print(f"runtime: {record['archive']} {record['sha256']}")
            else:
                manifest = json.loads(archive.read(prefix + "build-manifest.json"))
                if not isinstance(manifest, dict):
                    fail("archive build manifest must be an object")
                _verify_runtime_manifest(manifest, prefix, checksums, files)
                print("kind: runtime")
                _print_identity(manifest)
            print("SIZE SHA256 PATH")
            for name, digest in sorted(checksums.items()):
                print(f"{files[prefix + name].file_size} {digest} {name}")
            print(f"files: {len(checksums)}; SHA256SUMS: OK")
    except (zipfile.BadZipFile, KeyError, ValueError) as error:
        fail(f"cannot inspect archive {display_text(path)}: {error}")


def inspect_distribution(directory: Path) -> None:
    """Check all release assets and their shared corresponding-source pairing."""
    from fplinux_cli.dependencies.distribution import (  # noqa: PLC0415 -- native metadata reader.
        read_dependency_distribution,
    )
    from fplinux_cli.packaging.distribution import (  # noqa: PLC0415 -- distribution backend.
        distribution_matrix,
        source_distribution,
    )

    try:
        metadata = json.loads((directory / "RELEASE.json").read_bytes())
        if not isinstance(metadata, dict) or set(metadata) != {
            "release_id",
            "source_commit",
            "source_snapshot",
            "runtimes",
            "sources",
            "dependencies",
        }:
            fail("distribution has invalid release association metadata")
        context = release_context_from_fields(metadata).fields()
        dependency = read_dependency_distribution(directory)
        if (
            dependency.release.fields() != context
            or metadata["dependencies"] != "DEPENDENCIES.json"
        ):
            fail("distribution dependency inputs belong to another release")
        source_name = f"FPLinux-{context['release_id']}-sources.zip"
        if metadata["sources"] != {
            "archive": source_name,
            "sha256": sha256_file(directory / source_name),
        }:
            fail("distribution source archive differs from its release association")
        source = directory / source_name
        with zipfile.ZipFile(source) as archive:
            prefix, checksums, _files = _verify_archive(archive)
            records = _source_records(archive, prefix, checksums)
            source_context = release_context_from_fields(
                source_distribution(archive.read(prefix + "DISTRIBUTION.json"))
            ).fields()
        if source_context != context or metadata["runtimes"] != records:
            fail("distribution source/runtime association differs")
        expected_matrix = set(distribution_matrix())
        if {(record["target"], record["host"]) for record in records} != expected_matrix:
            fail("distribution does not contain the complete supported target/host matrix")
        expected_names = {
            source.name,
            "SHA256SUMS",
            "RELEASE.json",
            *dependency.files,
            *(record["archive"] for record in records),
        }
        if {path.name for path in directory.iterdir()} != expected_names:
            fail("distribution files do not match its runtime/source inventory")
        outer = "".join(
            f"{sha256_file(directory / name)}  {name}\n"
            for name in sorted(expected_names - {"SHA256SUMS"})
        )
        if (directory / "SHA256SUMS").read_text(encoding="ascii") != outer:
            fail("distribution SHA256SUMS does not match its final assets")
        for record in records:
            expected = (
                f"FPLinux-{record['target']}-{record['host']}-x64-{context['release_id']}.zip"
            )
            runtime = directory / record["archive"]
            if record["archive"] != expected or sha256_file(runtime) != record["sha256"]:
                fail(f"distribution runtime name or pairing hash differs: {record['archive']}")
            with zipfile.ZipFile(runtime) as archive:
                prefix, checksums, files = _verify_archive(archive)
                manifest = json.loads(archive.read(prefix + "build-manifest.json"))
                if not isinstance(manifest, dict):
                    fail("distribution runtime build manifest must be an object")
                _verify_runtime_manifest(manifest, prefix, checksums, files)
                if (
                    manifest.get("device_identity") != record["identity"]
                    or manifest.get("target") != record["target"]
                    or manifest.get("profile") is not None
                    or manifest.get("build_type") != "release"
                    or release_context_from_fields(manifest).fields() != context
                ):
                    fail(f"distribution runtime identity differs: {runtime.name}")
                readme = archive.read(prefix + "README.txt").decode("utf-8")
                if f"\n  {source.name}\n" not in readme:
                    fail(f"distribution runtime names another source companion: {runtime.name}")
                members = {name.removeprefix(prefix) for name in files}
                if members & {
                    "device-data.cpio",
                    "device-data-manifest.json",
                    "pinmap.bin",
                    "keymap.bin",
                } or any(name.startswith(("data/", "logs/", ".cache/")) for name in members):
                    fail(f"distribution runtime contains prepared personal data: {runtime.name}")
            print(f"runtime: {runtime.name}; pairing: OK")
        for name in expected_names - {"SHA256SUMS"}:
            if (directory / name).stat().st_size >= 2 * 1024**3:
                fail(f"distribution asset must be smaller than 2 GiB: {name}")
    except (OSError, zipfile.BadZipFile, KeyError, ValueError, AttributeError) as error:
        fail(f"cannot inspect distribution {display_text(directory)}: {error}")
    print(f"distribution: {len(records)} runtimes; corresponding sources and checksums: OK")


def inspect_apk(path: Path) -> None:
    """Read APK v2 control and data tar members, without validating package signatures."""
    try:
        # APK v2 joins gzip/tar streams for signature, control and data sections.
        with tarfile.open(path, "r:gz", ignore_zeros=True) as archive:
            metadata = archive.extractfile(".PKGINFO")
            if metadata is None:
                fail("APK has no regular .PKGINFO file")
            with metadata:
                text = metadata.read().decode("utf-8")
            print(f"apk: {display_text(path)}")
            print("metadata (.PKGINFO):")
            print(text, end="" if text.endswith("\n") else "\n")
            print("TYPE SIZE PATH")
            for member in archive:
                if member.isdir():
                    kind = "dir"
                elif member.issym():
                    kind = "symlink"
                elif member.islnk():
                    kind = "hardlink"
                else:
                    kind = "file" if member.isfile() else "special"
                suffix = f" -> {member.linkname}" if member.issym() or member.islnk() else ""
                print(f"{kind} {member.size} {member.name}{suffix}")
    except (tarfile.TarError, KeyError, UnicodeDecodeError, EOFError) as error:
        fail(f"cannot inspect APK {display_text(path)}: {error}")

# SPDX-License-Identifier: GPL-2.0-only
"""Combine runtime archives, corresponding sources and retained dependency inputs."""

from __future__ import annotations

import json
import re
import shutil
import stat
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, cast

from fplinux_cli import common
from fplinux_cli.artifacts.release import release_context_from_fields
from fplinux_cli.common import canonical_json_bytes, fail, is_sha256, sha256_bytes
from fplinux_cli.data_paths import DATA_DIR_NAME, PRIVATE_DIR_NAME
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.paths import discover_targets
from fplinux_cli.packaging.hosts import runtime_hosts

if TYPE_CHECKING:
    from collections.abc import Collection
    from typing import Any

_MAX_ASSET_BYTES = 2 * 1024**3
_SELECTION_METADATA = {
    "upstream/downloads.json",
    "upstream/alpine-source-channels.json",
}


def require_public_runtime_files(files: Collection[str], archive_name: str) -> None:
    """Keep installation state and prepared phone data out of a release payload."""
    private = {"device-data.cpio", "device-data-manifest.json", "pinmap.bin", "keymap.bin"}
    if private.intersection(files) or any(
        PurePosixPath(name).parts[0] in {"data", "logs", ".cache", DATA_DIR_NAME, PRIVATE_DIR_NAME}
        for name in files
    ):
        fail(f"distribution runtime contains prepared phone data: {archive_name}")


def distribution_matrix() -> tuple[tuple[str, str], ...]:
    """Discover each supported default RAM target and its ready runtime hosts."""
    return tuple(
        (target, host)
        for target in discover_targets()
        for host in runtime_hosts(targets.load_target(target))
    )


def _asset_size(path: Path) -> None:
    if path.stat().st_size >= _MAX_ASSET_BYTES:
        fail(f"distribution asset must be smaller than 2 GiB: {path.name}")


def _read_archive(path: Path) -> tuple[dict[str, bytes], set[str]]:
    """Admit only complete regular members with the producer's modes and checksums."""
    _asset_size(path)
    files: dict[str, bytes] = {}
    executables: set[str] = set()
    with zipfile.ZipFile(path) as archive:
        prefix = f"{path.stem}/"
        for item in archive.infolist():
            relative = item.filename.removeprefix(prefix)
            normalized = PurePosixPath(relative)
            mode = item.external_attr >> 16
            if (
                not item.filename.startswith(prefix)
                or not relative
                or normalized.is_absolute()
                or ".." in normalized.parts
                or normalized.as_posix() != relative
                or relative in files
                or mode not in {stat.S_IFREG | 0o644, stat.S_IFREG | 0o755}
            ):
                fail(f"distribution input has an invalid member: {path.name}: {item.filename}")
            files[relative] = archive.read(item)
            if mode & 0o111:
                executables.add(relative)
    checksums = files.get("SHA256SUMS", b"").decode("utf-8")
    expected = "".join(
        f"{sha256_bytes(files[name])}  {name}\n" for name in sorted(files) if name != "SHA256SUMS"
    )
    if not expected or checksums != expected:
        fail(f"distribution input has an invalid checksum inventory: {path.name}")
    return files, executables


def source_distribution(data: bytes) -> dict[str, Any]:
    """Read source metadata without discarding its common release context."""
    try:
        metadata = json.loads(data)
    except ValueError as error:
        fail(f"invalid source distribution metadata: {error}")
    if (
        not isinstance(metadata, dict)
        or set(metadata) != {"release_id", "source_commit", "source_snapshot", "runtimes"}
        or not isinstance(metadata["runtimes"], list)
        or not metadata["runtimes"]
    ):
        fail("source distribution must contain a nonempty runtimes list")
    release_context_from_fields(metadata)
    return cast("dict[str, Any]", metadata)


def distribution_records(data: bytes) -> list[dict[str, str]]:
    """Parse the single source-to-runtime contract used by packaging and inspection."""
    metadata = source_distribution(data)
    records = []
    archives: set[str] = set()
    selections: set[tuple[str, str]] = set()
    for record in metadata["runtimes"]:
        if (
            not isinstance(record, dict)
            or set(record) != {"archive", "target", "host", "identity", "sha256"}
            or not all(isinstance(value, str) for value in record.values())
            or record["host"] not in {"linux", "windows"}
            or not is_sha256(record["identity"])
            or not is_sha256(record["sha256"])
            or re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", record["target"]) is None
            or not record["archive"].endswith(".zip")
            or len(PurePosixPath(record["archive"]).parts) != 1
            or PurePosixPath(record["archive"]).name != record["archive"]
            or "\\" in record["archive"]
            or "\0" in record["archive"]
        ):
            fail("source distribution has an invalid runtime record")
        selection = record["target"], record["host"]
        if record["archive"] in archives or selection in selections:
            fail("source distribution repeats a runtime archive or target/host selection")
        archives.add(record["archive"])
        selections.add(selection)
        records.append(cast("dict[str, str]", record))
    return records


def _source_record(path: Path) -> tuple[dict[str, str], dict[str, str | None]]:
    with zipfile.ZipFile(path) as archive:
        data = archive.read(f"{path.stem}/DISTRIBUTION.json")
        records = distribution_records(data)
    if len(records) != 1:
        fail(f"distribution input must describe one runtime: {path.name}")
    return records[0], release_context_from_fields(source_distribution(data)).fields()


def _merge_sources(  # noqa: PLR0913 -- member payloads, modes and selection remain explicit.
    destination: dict[str, bytes],
    destination_executables: set[str],
    files: dict[str, bytes],
    executables: set[str],
    *,
    target: str,
    host: str,
) -> None:
    """Share common bytes while preserving each selection's variable source material."""
    for name, contents in files.items():
        if name in {"DISTRIBUTION.json", "SHA256SUMS"}:
            continue
        member = name
        if name in _SELECTION_METADATA or name.startswith("sources/"):
            member = f"selections/{target}/{host}/{name}"
        if member in destination and (
            destination[member] != contents
            or (member in destination_executables) != (name in executables)
        ):
            fail(f"conflicting corresponding source member: {member}")
        destination[member] = contents
        if name in executables:
            destination_executables.add(member)


def _final_runtime(
    archive: Path, record: dict[str, str], *, source_stem: str, context: dict[str, str | None]
) -> tuple[dict[str, bytes], set[str], dict[str, Any]]:
    from fplinux_cli.packaging.archive import (  # noqa: PLC0415 -- Linux assembly backend.
        distribution_record,
        finalize_runtime,
    )
    from fplinux_cli.packaging.readme import (  # noqa: PLC0415 -- Linux assembly backend.
        append_source_companion,
    )

    files, executables = _read_archive(archive)
    manifest = json.loads(files["build-manifest.json"])
    if (
        not isinstance(manifest, dict)
        or manifest.get("target") != record["target"]
        or manifest.get("device_identity") != record["identity"]
        or manifest.get("build_type") != "release"
        or manifest.get("profile") is not None
        or release_context_from_fields(manifest).fields() != context
    ):
        fail(f"distribution runtime has the wrong build identity: {archive.name}")
    if distribution_record(archive, manifest, record["host"]) != record:
        fail(f"corresponding sources do not match runtime: {archive.name}")
    original = dict(files)
    finalize_runtime(files, manifest, executables)
    if files != original:
        fail(f"distribution runtime has an invalid build manifest: {archive.name}")
    require_public_runtime_files(files, archive.name)
    launcher = "Run.cmd" if record["host"] == "windows" else "Run.sh"
    if launcher not in files:
        fail(f"distribution runtime has no launcher: {archive.name}")
    companion = append_source_companion(b"", f"{archive.stem}-sources")
    if not files["README.txt"].endswith(companion):
        fail(f"distribution runtime does not name its matching sources: {archive.name}")
    files["README.txt"] = append_source_companion(
        files["README.txt"].removesuffix(companion), source_stem
    )
    finalize_runtime(files, manifest, executables)
    return files, executables, manifest


def assemble_distribution(
    input_directory: Path, output_directory: Path, *, inputs_directory: Path
) -> Path:
    """Publish the exact supported runtime matrix, one source ZIP and outer checksums."""
    if output_directory.exists():
        fail(f"distribution output must be a new directory: {output_directory}")
    try:
        return _assemble_distribution(
            input_directory, output_directory, inputs_directory=inputs_directory
        )
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        fail(f"cannot assemble distribution: {error}")


def _assemble_distribution(
    input_directory: Path, output_directory: Path, *, inputs_directory: Path
) -> Path:
    from fplinux_cli.dependencies.distribution import (  # noqa: PLC0415 -- transport validation.
        read_dependency_distribution,
    )
    from fplinux_cli.packaging.archive import (  # noqa: PLC0415 -- Linux assembly backend.
        _write_archive,
        distribution_record,
        finalize_checksums,
    )

    dependency = read_dependency_distribution(inputs_directory)
    context = dependency.release.fields()
    release_id = context["release_id"]
    expected = set(distribution_matrix())
    if not expected:
        fail("distribution has no supported target/host selections")
    companions: dict[tuple[str, str], tuple[Path, dict[str, str]]] = {}
    runtime_paths = set(input_directory.glob("*.zip"))
    for path in sorted(runtime_paths):
        if not path.name.endswith("-sources.zip"):
            continue
        record, source_context = _source_record(path)
        if source_context != context:
            fail(f"source companion belongs to another release: {path.name}")
        selection = record["target"], record["host"]
        if selection in companions:
            fail(f"duplicate corresponding sources for {selection[0]}/{selection[1]}")
        companions[selection] = path, record
    if set(companions) != expected:
        missing = sorted(expected - companions.keys())
        extra = sorted(companions.keys() - expected)
        fail(
            "distribution target/host matrix is incomplete or unexpected: "
            f"missing={missing}, extra={extra}"
        )
    names = set()
    for (target, host), (source, record) in companions.items():
        runtime = f"FPLinux-{target}-{host}-x64-{release_id}.zip"
        if record["archive"] != runtime or source.name != f"{Path(runtime).stem}-sources.zip":
            fail(f"distribution input has the wrong build name: {source.name}")
        names.update({source.name, runtime})
    if {path.name for path in runtime_paths} != names:
        fail("distribution inputs do not contain exactly one runtime and companion per selection")
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    source_stem = f"FPLinux-{release_id}-sources"
    with tempfile.TemporaryDirectory(
        dir=output_directory.parent, prefix=".fplinux-distribution-"
    ) as temporary:
        staging = Path(temporary) / "assets"
        staging.mkdir()
        sources: dict[str, bytes] = {}
        source_executables: set[str] = set()
        records = []
        for (target, host), (companion, record) in sorted(companions.items()):
            files, executables, manifest = _final_runtime(
                input_directory / record["archive"],
                record,
                source_stem=source_stem,
                context=context,
            )
            final_archive = _write_archive(
                staging,
                Path(record["archive"]).stem,
                files,
                executables,
                compression=zipfile.ZIP_DEFLATED,
            )
            _asset_size(final_archive)
            records.append(distribution_record(final_archive, manifest, host))
            source_files, executable_sources = _read_archive(companion)
            _merge_sources(
                sources,
                source_executables,
                source_files,
                executable_sources,
                target=target,
                host=host,
            )
        sources["DISTRIBUTION.json"] = canonical_json_bytes({**context, "runtimes": records})
        finalize_checksums(sources)
        source_archive = _write_archive(
            staging, source_stem, sources, source_executables, compression=zipfile.ZIP_DEFLATED
        )
        _asset_size(source_archive)
        for name in dependency.files:
            shutil.copyfile(inputs_directory / name, staging / name)
            _asset_size(staging / name)
        association = {
            **context,
            "runtimes": records,
            "sources": {
                "archive": source_archive.name,
                "sha256": common.sha256_file(source_archive),
            },
            "dependencies": "DEPENDENCIES.json",
        }
        common.replace_file_atomically(
            staging / "RELEASE.json", canonical_json_bytes(association), 0o644
        )
        checksums = "".join(
            f"{common.sha256_file(path)}  {path.name}\n" for path in sorted(staging.iterdir())
        ).encode()
        common.replace_file_atomically(staging / "SHA256SUMS", checksums, 0o644)
        staging.replace(output_directory)
    return output_directory / source_archive.name

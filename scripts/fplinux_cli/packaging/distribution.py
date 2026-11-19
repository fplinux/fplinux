# SPDX-License-Identifier: GPL-2.0-only
"""Build one set of standalone archives and bind it to its sources."""

from __future__ import annotations

import json
import re
import shutil
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, cast

from fplinux_cli import common
from fplinux_cli.artifacts.release import capture_public_sources, release_context_from_fields
from fplinux_cli.common import canonical_json_bytes, fail, is_sha256
from fplinux_cli.data_paths import DATA_DIR_NAME, PRIVATE_DIR_NAME, data_root
from fplinux_cli.manifests import releases, targets
from fplinux_cli.manifests.paths import discover_targets
from fplinux_cli.packaging.card_kit import read_card_files, supports_release_card
from fplinux_cli.packaging.hosts import runtime_hosts
from fplinux_cli.packaging.universal import HostArchive, release_archive_name
from fplinux_cli.runtime.bundle import MICROSD_BOOT_PROFILE

if TYPE_CHECKING:
    from collections.abc import Collection
    from typing import Any

    from fplinux_cli.artifacts.release import ReleaseContext
    from fplinux_cli.packaging.sources import SourcePayload

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
    """Discover the default RAM archives included in automatic releases."""
    return tuple(
        (target, host)
        for target in discover_targets()
        if releases.load_release(target)["publish"]
        for host in runtime_hosts(targets.load_target(target))
    )


def _asset_size(path: Path) -> None:
    if path.stat().st_size >= _MAX_ASSET_BYTES:
        fail(f"distribution asset must be smaller than 2 GiB: {path.name}")


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
    archives: dict[str, tuple[str, str]] = {}
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
        if selection in selections:
            fail("source distribution repeats a runtime target/host selection")
        association = record["host"], record["sha256"]
        if archives.setdefault(record["archive"], association) != association:
            fail("source distribution gives conflicting identities for one runtime archive")
        selections.add(selection)
        records.append(cast("dict[str, str]", record))
    return records


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


def _write_metadata(
    directory: Path,
    metadata: dict[str, Any],
) -> None:
    for path in directory.iterdir():
        _asset_size(path)
    common.replace_file_atomically(
        directory / "RELEASE.json", canonical_json_bytes(metadata), 0o644
    )
    checksums = "".join(
        f"{common.sha256_file(path)}  {path.name}\n"
        for path in sorted(directory.iterdir())
        if path.name != "SHA256SUMS"
    )
    common.replace_file_atomically(directory / "SHA256SUMS", checksums.encode(), 0o644)


def _publish_directory(staging: Path, destination: Path) -> None:
    if not destination.exists() and not destination.is_symlink():
        staging.rename(destination)
        return
    if destination.is_symlink() or not destination.is_dir():
        fail(f"invalid package output directory: {destination}")
    expected = {path.name for path in staging.iterdir()}
    if {path.name for path in destination.iterdir()} != expected or any(
        (path := destination / name).is_symlink()
        or not path.is_file()
        or common.sha256_file(path) != common.sha256_file(staging / name)
        for name in expected
    ):
        fail(f"package output directory contains a different release: {destination}")


def _write_distribution(
    output: Path,
    release: ReleaseContext,
    archives: dict[str, HostArchive],
    identities: dict[tuple[str, str], str],
    sources: SourcePayload,
) -> None:
    from fplinux_cli.cli.inspect import inspect_distribution  # noqa: PLC0415
    from fplinux_cli.packaging.archive import _write_archive, finalize_checksums  # noqa: PLC0415

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".fplinux-package-") as temporary:
        staging = Path(temporary) / "release"
        staging.mkdir()
        source_stem = f"FPLinux-{release.release_id}-sources"
        runtimes = {}
        for host, payload in archives.items():
            finalize_checksums(payload.files)
            archive = _write_archive(
                staging,
                Path(release_archive_name(host, release.release_id)).stem,
                payload.files,
                payload.executables,
                compression=zipfile.ZIP_DEFLATED,
            )
            runtimes[host] = {"archive": archive.name, "sha256": common.sha256_file(archive)}
        records = [
            {"target": target, "host": host, "identity": identity, **runtimes[host]}
            for (target, host), identity in sorted(identities.items())
        ]
        context = release.fields()
        sources.files["DISTRIBUTION.json"] = canonical_json_bytes({**context, "runtimes": records})
        finalize_checksums(sources.files)
        source_archive = _write_archive(
            staging,
            source_stem,
            sources.files,
            sources.executables,
            compression=zipfile.ZIP_DEFLATED,
        )
        _write_metadata(
            staging,
            {
                **context,
                "runtimes": records,
                "sources": {
                    "archive": source_archive.name,
                    "sha256": common.sha256_file(source_archive),
                },
                "dependencies": None,
            },
        )
        inspect_distribution(staging)
        _publish_directory(staging, output)


def _package_selections(target: str | None, host: str | None) -> tuple[tuple[str, str], ...]:
    if target is None:
        selections = distribution_matrix()
    else:
        selections = tuple((target, name) for name in runtime_hosts(targets.load_target(target)))
    if host is not None:
        selections = tuple(selection for selection in selections if selection[1] == host)
    if not selections:
        fail(
            f"no supported phone and host combination selected: {target or 'all'}/{host or 'both'}"
        )
    return selections


def package_release(
    *,
    target: str | None,
    host: str | None,
    jobs: int,
    output_directory: Path | None = None,
) -> None:
    """Build exact release inputs and deliver one ZIP format for any selection."""
    from itertools import groupby  # noqa: PLC0415 -- grouping is only needed on the build host.

    from fplinux_cli.build.workflow import build  # noqa: PLC0415 -- Linux build backend.
    from fplinux_cli.packaging.archive import (  # noqa: PLC0415
        finalize_runtime,
        project_bundle,
    )
    from fplinux_cli.packaging.autonomous import HostPayloads  # noqa: PLC0415
    from fplinux_cli.packaging.sources import (  # noqa: PLC0415
        collect_source_files,
        project_source_files,
    )

    selections = _package_selections(target, host)
    cache = data_root(common.ROOT)
    snapshot = capture_public_sources(selected_data_root=cache)
    release = snapshot.release
    archives = {
        name: HostArchive(name, release.release_id) for name in sorted({h for _, h in selections})
    }
    sources = project_source_files(snapshot)
    identities = {}
    native = HostPayloads(cache)
    for name, choices in groupby(sorted(selections), key=lambda selection: selection[0]):
        configuration = targets.load_target(name)
        bundle, manifest = build(name, jobs, include_device_data=False, release_context=release)
        card_config = None
        card_files = None
        card_build = None
        if supports_release_card(name):
            card_config = targets.load_target(name, MICROSD_BOOT_PROFILE)
            card_build = build(
                name,
                jobs,
                profile=MICROSD_BOOT_PROFILE,
                include_device_data=False,
                release_context=release,
            )
            card_files = read_card_files(card_build[0].path, card_build[1], release)
        for _, selected_host in choices:
            files, modes, tool_sources = project_bundle(
                name,
                configuration,
                bundle.path,
                manifest,
                host=selected_host,
                host_payloads=native,
                card_files=card_files,
            )
            source = collect_source_files(
                configuration, host=selected_host, cache=cache, card_config=card_config
            )
            files.update(source.notices)
            require_public_runtime_files(files.keys(), name)
            finalize_runtime(files, manifest, modes)
            microsd = None
            if card_config is not None and card_build is not None:
                card_bundle, card_manifest = card_build
                sd_files, sd_modes, sd_sources = project_bundle(
                    name,
                    card_config,
                    card_bundle.path,
                    card_manifest,
                    host=selected_host,
                    host_payloads=native,
                )
                sd_files.update(source.notices)
                tool_sources.update(sd_sources)
                require_public_runtime_files(sd_files.keys(), name)
                finalize_runtime(sd_files, card_manifest, sd_modes)
                microsd = sd_files, sd_modes
            archives[selected_host].add(name, files, modes, microsd=microsd)
            source.files.update(tool_sources)
            _merge_sources(
                sources.files,
                sources.executables,
                source.files,
                source.executables,
                target=name,
                host=selected_host,
            )
            identities[(name, selected_host)] = manifest["device_identity"]
    for archive in archives.values():
        archive.finish(f"FPLinux-{release.release_id}-sources")
    if capture_public_sources(selected_data_root=cache).release != release:
        fail("release sources changed while packaging; rerun package")
    output = output_directory or cache / "out/releases" / release.release_id / (
        target or "all"
    ) / (host or "both")
    _write_distribution(output, release, archives, identities, sources)
    print(f"FPLinux packages: {common.display_text(output)}")


def attach_inputs(directory: Path, inputs: Path, output: Path) -> None:
    """Add the release's dependency transport without changing its archive bytes."""
    from fplinux_cli.cli.inspect import inspect_distribution  # noqa: PLC0415
    from fplinux_cli.dependencies.distribution import read_dependency_distribution  # noqa: PLC0415

    metadata = inspect_distribution(directory, complete=True)
    if metadata["dependencies"] is not None:
        fail("package already includes a dependency transport")
    dependency = read_dependency_distribution(inputs)
    if release_context_from_fields(metadata) != dependency.release:
        fail("dependency transport does not match the packaged release")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".fplinux-release-") as temporary:
        staging = Path(temporary) / "release"
        staging.mkdir()
        for path in directory.iterdir():
            if path.name not in {"RELEASE.json", "SHA256SUMS"}:
                shutil.copyfile(path, staging / path.name)
        for name in dependency.files:
            shutil.copyfile(inputs / name, staging / name)
        _write_metadata(staging, {**metadata, "dependencies": "DEPENDENCIES.json"})
        inspect_distribution(staging, complete=True, require_inputs=True)
        _publish_directory(staging, output)

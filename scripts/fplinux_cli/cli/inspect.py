# SPDX-License-Identifier: GPL-2.0-only
"""Inspect published artifacts without building, extracting or executing them."""

from __future__ import annotations

import json
import tarfile
from typing import TYPE_CHECKING, Any

from fplinux_cli.artifacts.footprint import FootprintError, compare_footprints, inspect_footprint
from fplinux_cli.artifacts.release import current_release_context, release_context_from_fields
from fplinux_cli.common import display_text, fail, sha256_file
from fplinux_cli.runtime.bundle_session import resolve_target_bundle

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path


def _print_release(manifest: Mapping[str, object]) -> None:
    release = release_context_from_fields(manifest)
    print(f"release_id: {release.release_id}")
    print(f"source_commit: {release.source_commit or '-'}")
    print(f"source_snapshot: {release.source_snapshot}")


def _print_identity(manifest: Mapping[str, object]) -> None:
    """Show the identity recorded in the inspected build, not the current source checkout."""
    _print_release(manifest)
    for key in ("target", "profile", "build_type", "generation"):
        value = manifest.get(key)
        if key == "profile" and value is None:
            value = "default"
        if not isinstance(value, str) or not value:
            fail(f"build manifest has no valid {key}")
        print(f"{key}: {value}")


def inspect_source() -> None:
    """Report current public source identity without preparing a build."""
    _print_release(current_release_context().fields())


def inspect_bundle(  # noqa: PLR0913 -- build context and phone data selection stay explicit.
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    current_source: bool = False,
    include_device_data: bool = True,
    phone_data: Path | None = None,
) -> None:
    """Read the selected current generation and compare its files to its build manifest."""
    if not current_source and (not include_device_data or phone_data is not None):
        fail("--phone-data and --without-device-data require --current-source")
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
    if current_source:
        _compare_current_bundle(
            manifest,
            target,
            profile=profile,
            build_type=build_type,
            include_device_data=include_device_data,
            phone_data=phone_data,
        )


def _compare_current_bundle(  # noqa: PLR0913 -- build context and phone selection remain explicit.
    manifest: dict[str, object],
    target: str,
    *,
    profile: str | None,
    build_type: str,
    include_device_data: bool,
    phone_data: Path | None,
) -> None:
    from fplinux_cli.alpine.signing import signing_key_identity  # noqa: PLC0415
    from fplinux_cli.build.reuse import build_identity, manifest_matches_identity  # noqa: PLC0415
    from fplinux_cli.common import ROOT  # noqa: PLC0415
    from fplinux_cli.data_paths import data_root  # noqa: PLC0415
    from fplinux_cli.environment.definition import (  # noqa: PLC0415
        environment_recipe_digest,
        load_environment_lock,
    )
    from fplinux_cli.environment.runtime import environment_rootfs  # noqa: PLC0415
    from fplinux_cli.environment.state import load_environment_state  # noqa: PLC0415
    from fplinux_cli.workspace.build_inputs import target_workspace_snapshot  # noqa: PLC0415

    cache = data_root(ROOT)
    release = current_release_context(selected_data_root=cache)
    snapshot = target_workspace_snapshot(
        target,
        profile,
        build_type=build_type,
        include_device_data=include_device_data,
        release_context=release,
        phone_data=phone_data,
        selected_data_root=cache,
    )
    lock = load_environment_lock()
    state = load_environment_state(cache, environment_recipe_digest(lock))
    if state is not None and not environment_rootfs(cache, state).is_dir():
        state = None
    try:
        signing = signing_key_identity(cache)
    except SystemExit:
        signing = None
    comparisons = {
        "source": all(manifest.get(key) == value for key, value in release.fields().items()),
        "workspace": manifest.get("workspace_digest") == snapshot.recipe,
        "environment": None
        if state is None
        else (
            manifest.get("environment_recipe") == state.recipe
            and manifest.get("environment_content") == state.content
        ),
        "signing": None if signing is None else manifest.get("apk_signing_key") == signing,
    }
    for name, matches in comparisons.items():
        print(f"{name}: {'unavailable' if matches is None else 'current' if matches else 'stale'}")
    identity = build_identity(snapshot, state, cache)
    if False in comparisons.values():
        result = "stale"
    elif None in comparisons.values():
        result = "unavailable"
    elif manifest_matches_identity(manifest, identity):
        result = "current"
    else:
        result = "stale"
    print("environment identity uses the saved receipt; installed content was not probed")
    print(f"bundle freshness: {result}")


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


def inspect_archive(path: Path) -> None:
    """Report the verified contents of a release ZIP."""
    from fplinux_cli.packaging.verification import (  # noqa: PLC0415 -- ZIP inspection only.
        SourceArchive,
        verify_archive,
    )

    report = verify_archive(path)
    print(f"archive: {display_text(path)}")
    if isinstance(report, SourceArchive):
        print("kind: corresponding-sources")
        _print_release(report.release.fields())
        for name, digest in dict.fromkeys(
            (row["archive"], row["sha256"]) for row in report.runtimes
        ):
            print(f"runtime: {name} {digest}")
    else:
        print("kind: universal runtime")
        print(f"release: {report.release_id}")
        for target in report.targets:
            print(f"target: {target}")
    print("SIZE SHA256 PATH")
    for name, (size, digest) in sorted(report.files.items()):
        print(f"{size} {digest} {name}")
    print(f"files: {len(report.files)}; SHA256SUMS: OK")


def inspect_distribution(directory: Path) -> dict[str, Any]:
    """Report the verified associations of the release assets."""
    from fplinux_cli.packaging.distribution import print_distribution_report  # noqa: PLC0415
    from fplinux_cli.packaging.verification import verify_distribution  # noqa: PLC0415

    metadata = verify_distribution(directory)
    print_distribution_report(metadata)
    return metadata


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

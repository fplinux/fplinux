# SPDX-License-Identifier: GPL-2.0-only
"""Regenerate checksums for one selected Alpine aport."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.alpine import selection as alpine_selection
from fplinux_cli.alpine.aports import materialize_aport_sources
from fplinux_cli.alpine.sources import (
    aport_declaration,
    aport_sources,
    checksum_block,
    replace_checksum_block,
)
from fplinux_cli.build import workflow as build_commands
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.common import fail, replace_file_atomically
from fplinux_cli.dependencies.inputs import acquire_inputs, aport_source_inputs
from fplinux_cli.environment import kern as environment_kern
from fplinux_cli.environment import runtime as environment_runtime
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace import staging
from fplinux_cli.workspace.build_inputs import add_source_path
from fplinux_cli.workspace.capture import WorkspaceSnapshot, workspace_snapshot


def _canonical_aport(package: str) -> Path:
    """Return one regular canonical aport selected by a package identifier."""
    if alpine_lock.PACKAGE_ID.fullmatch(package) is None:
        fail(f"invalid Alpine package identifier: {package}")
    aport = common.ROOT / "alpine/aports" / package
    if aport.is_symlink() or not aport.is_dir():
        fail(f"Alpine aport is missing or invalid: {aport}")
    apkbuild = aport / "APKBUILD"
    if apkbuild.is_symlink() or not apkbuild.is_file():
        fail(f"Alpine aport has no regular APKBUILD: {aport}")
    return aport


def _aport_checksum_snapshot(package: str) -> WorkspaceSnapshot:
    """Capture the exact canonical files that ``abuild checksum`` may consume."""
    aport = _canonical_aport(package)
    files: dict[str, Path] = {}
    add_source_path(files, aport)
    for source in alpine_selection.shared_aport_sources(package, root=common.ROOT):
        add_source_path(files, source)
    add_source_path(files, common.ROOT / "scripts/fplinux_cli/alpine/checksum")
    add_source_path(files, common.ROOT / "sources.lock.toml")
    for relative in (
        "scripts/fplinux_cli/alpine/aports.py",
        "scripts/fplinux_cli/alpine/sources.py",
        "scripts/fplinux_cli/manifests/source_catalog.py",
    ):
        add_source_path(files, common.ROOT / relative)
    if package == "fplinux-ui":
        add_source_path(files, common.ROOT / "package-lock.json")
    return workspace_snapshot(sorted(files.items()))


def _checksum_block_bounds(data: bytes, *, path: Path) -> tuple[int, int] | None:
    """Locate the multiline or initially empty ``sha512sums`` assignment."""
    offset = 0
    start: int | None = None
    for line in data.splitlines(keepends=True):
        if line.rstrip(b"\r\n") == b'sha512sums=""':
            return offset, offset + len(line)
        if line.rstrip(b"\r\n") == b'sha512sums="':
            if start is not None:
                fail(f"APKBUILD has multiple sha512sums blocks: {path}")
            start = offset
        elif start is not None and line.rstrip(b"\r\n") == b'"':
            return start, offset + len(line)
        offset += len(line)
    if start is not None:
        fail(f"APKBUILD has an unterminated sha512sums block: {path}")
    return None


def _checksum_block_only_changed(before: bytes, after: bytes, *, path: Path) -> None:
    """Reject an ``abuild checksum`` result that changed non-checksum recipe text."""
    before_bounds = _checksum_block_bounds(before, path=path)
    after_bounds = _checksum_block_bounds(after, path=path)
    if after_bounds is None:
        fail(f"abuild checksum did not produce a sha512sums block: {path}")
    before_without = (
        before
        if before_bounds is None
        else before[: before_bounds[0]] + before[before_bounds[1] :]
    )
    after_without = after[: after_bounds[0]] + after[after_bounds[1] :]
    if before_without != after_without:
        fail(f"abuild checksum changed recipe text outside sha512sums: {path}")


def _checksum_container_command(
    kern: str,
    *,
    package: str,
    rootfs: Path,
    stage: Path,
    downloads: Path,
) -> list[str]:
    """Return the isolated build-environment invocation for one checksum regeneration."""
    stage_aport = f"/workspace/alpine/aports/{package}"
    return [
        kern,
        "box",
        environment_kern.kern_box_name("checksum"),
        *environment_runtime.environment_arguments(rootfs),
        "--read-only",
        "--uid-range",
        "--network",
        "none",
        "--tmpfs",
        "/tmp:1g",  # noqa: S108 -- container tmpfs.
        "--volume",
        f"{downloads}:/cache/downloads",
        "--volume",
        f"{stage}:/workspace",
        "--workdir",
        stage_aport,
        "--env",
        "HOME=/tmp/fplinux-home",
        "--init",
        "--quiet",
        "--",
        "sh",
        "/workspace/checksum/prepare.sh",
        "/workspace",
        "/cache/downloads",
        package,
        SOURCE_DATE_EPOCH,
    ]


def checksum_aport(package: str) -> None:
    """Regenerate one aport's SHA-512 block from its exact canonical source closure."""
    aport = _canonical_aport(package)
    apkbuild = aport / "APKBUILD"
    before_snapshot = _aport_checksum_snapshot(package)
    before_apkbuild = apkbuild.read_bytes()
    mode = apkbuild.stat().st_mode & 0o777
    reporter = RunReporter.create("checksum", target=package, verbose=False)
    kern, rootfs, _state = environment_runtime.require_environment()
    cache = common.ROOT / ".cache"
    downloads = build_commands.ensure_build_directory(cache / "downloads")
    with reporter.stage("source-inputs"):
        acquire_inputs(aport_source_inputs(common.ROOT, (package,)), cache, offline=True)
    with reporter.stage("workspace"):
        snapshot_root = staging.stage_workspace_snapshot(before_snapshot)
    checksum_root = build_commands.ensure_build_directory(cache / "checksum")
    with tempfile.TemporaryDirectory(
        dir=checksum_root,
        prefix=f"{package}-{before_snapshot.recipe[:12]}-",
    ) as temporary_root:
        stage = Path(temporary_root)
        stage_aport = stage / "alpine/aports" / package
        stage_aport.parent.mkdir(parents=True)
        materialize_aport_sources(
            package, snapshot_root, stage_aport, cache=cache, verify_local=False
        )
        materialized_apkbuild = (stage_aport / "APKBUILD").read_bytes()
        shutil.copytree(snapshot_root / "scripts/fplinux_cli/alpine/checksum", stage / "checksum")
        with reporter.stage("container", passthrough=True, show_tail=False) as stage_report:
            stage_report.run(
                _checksum_container_command(
                    kern,
                    package=package,
                    rootfs=rootfs,
                    stage=stage,
                    downloads=downloads,
                ),
                env=environment_kern.kern_environment(),
            )
        generated = stage / "alpine/aports" / package / "APKBUILD"
        if generated.is_symlink() or not generated.is_file():
            fail(f"abuild checksum did not leave a regular APKBUILD: {generated}")
        generated_apkbuild = generated.read_bytes()
        _checksum_block_only_changed(materialized_apkbuild, generated_apkbuild, path=generated)
        native = checksum_block(generated_apkbuild, path=generated)
        if native is None:
            fail(f"abuild checksum did not produce checksums: {package}")
        originals = {source.name for source in aport_sources(snapshot_root, package)}
        local_names = [
            name
            for name in aport_declaration(snapshot_root, package)["sources"]
            if name not in originals
        ]
        if any(name not in native[2] for name in local_names):
            fail(f"abuild checksum omitted a local source: {package}")
        generated_apkbuild = replace_checksum_block(
            before_apkbuild, {name: native[2][name] for name in local_names}, path=apkbuild
        )
    if _aport_checksum_snapshot(package).recipe != before_snapshot.recipe:
        fail(f"Alpine aport source closure changed while checksums were generated: {package}")
    if apkbuild.is_symlink() or not apkbuild.is_file() or apkbuild.read_bytes() != before_apkbuild:
        fail(f"canonical APKBUILD changed while checksums were generated: {apkbuild}")
    _checksum_block_only_changed(before_apkbuild, generated_apkbuild, path=apkbuild)
    replace_file_atomically(apkbuild, generated_apkbuild, mode)
    print(f"checksum {package}: OK", flush=True)
    reporter.finish()

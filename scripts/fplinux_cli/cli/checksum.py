# SPDX-License-Identifier: GPL-2.0-only
"""Regenerate checksums for one selected Alpine aport."""

from __future__ import annotations

import shlex
import tempfile
from pathlib import Path

from fplinux_cli import alpine_state, common
from fplinux_cli import workspace as workspaces
from fplinux_cli.alpine_builder import materialize_aport_sources
from fplinux_cli.build_env import SOURCE_DATE_EPOCH
from fplinux_cli.cli import build as build_commands
from fplinux_cli.common import fail, replace_file_atomically
from fplinux_cli.environment import images
from fplinux_cli.environment import kern as kern_env
from fplinux_cli.output import RunReporter
from fplinux_cli.workspace import WorkspaceSnapshot, add_source_path, workspace_snapshot


def _canonical_aport(package: str) -> Path:
    """Return one regular canonical aport selected by a package identifier."""
    if alpine_state.PACKAGE_ID.fullmatch(package) is None:
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
    for source in alpine_state.shared_aport_sources(package, root=common.ROOT):
        add_source_path(files, source)
    return workspace_snapshot(sorted(files.items()))


def _checksum_block_bounds(data: bytes, *, path: Path) -> tuple[int, int] | None:
    """Locate the one simple multiline ``sha512sums`` assignment in an APKBUILD."""
    offset = 0
    start: int | None = None
    for line in data.splitlines(keepends=True):
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


def _checksum_container_command(  # noqa: PLR0913
    kern: str,
    *,
    package: str,
    image: str,
    offline: bool,
    stage: Path,
    downloads: Path,
) -> list[str]:
    """Return the isolated pinned-image invocation for one checksum regeneration."""
    source_cache = "/cache/downloads/alpine/sources"
    stage_aport = f"/workspace/alpine/aports/{package}"
    builder_command = "\n".join(
        (
            "set -eu",
            "export "
            + " ".join(
                shlex.quote(value)
                for value in (
                    f"SRCDEST={source_cache}",
                    f"SOURCE_DATE_EPOCH={SOURCE_DATE_EPOCH}",
                )
            ),
            "abuild checksum",
            "exec apkbuild-lint APKBUILD",
        )
    )
    prepare_command = "\n".join(
        (
            "chmod 755 /workspace /workspace/alpine /workspace/alpine/aports",
            f"install -d -o builder -g builder {source_cache}",
            f"restore_stage() {{ chown -R 0:0 {stage_aport}; }}",
            "trap restore_stage EXIT HUP INT TERM",
            f"chown -R builder:builder {stage_aport}",
            "su builder -s /bin/sh -c " + shlex.quote(builder_command),
        )
    )
    return [
        kern,
        "box",
        kern_env.kern_box_name("checksum"),
        "--image",
        image,
        "--pull",
        "never",
        "--read-only",
        "--network",
        "none" if offline else "host",
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
        "-ceu",
        prepare_command,
    ]


def checksum_aport(package: str, *, offline: bool = False) -> None:
    """Regenerate one aport's SHA-512 block from its exact canonical source closure."""
    aport = _canonical_aport(package)
    apkbuild = aport / "APKBUILD"
    before_snapshot = _aport_checksum_snapshot(package)
    before_apkbuild = apkbuild.read_bytes()
    mode = apkbuild.stat().st_mode & 0o777
    container_lock = images.load_container_lock()
    image_recipe = images.container_image_recipe_digest(container_lock)
    image = images.container_image_reference(container_lock, image_recipe)
    reporter = RunReporter.create("checksum", target=package, verbose=False)
    if not kern_env.kern_available(container_lock):
        if offline:
            fail(
                "offline checksum requires the current pinned OCI image; "
                "run ./fplinux setup online first"
            )
        kern_env.setup(reporter=reporter, lock=container_lock, image_recipe=image_recipe)
    kern = kern_env.require_kern(container_lock)
    if kern_env.current_image_state(kern, image, image_recipe) is None:
        if offline:
            fail(
                "offline checksum requires the current pinned OCI image; "
                "run ./fplinux setup online first"
            )
        kern_env.setup(reporter=reporter, lock=container_lock, image_recipe=image_recipe)
    cache = common.ROOT / ".cache"
    downloads = build_commands.ensure_build_directory(cache / "downloads")
    with reporter.stage("workspace"):
        snapshot_root = workspaces.stage_workspace_snapshot(before_snapshot)
    checksum_root = build_commands.ensure_build_directory(cache / "checksum")
    with tempfile.TemporaryDirectory(
        dir=checksum_root,
        prefix=f"{package}-{before_snapshot.recipe[:12]}-",
    ) as temporary_root:
        stage = Path(temporary_root)
        stage_aport = stage / "alpine/aports" / package
        stage_aport.parent.mkdir(parents=True)
        materialize_aport_sources(package, snapshot_root, stage_aport)
        with reporter.stage("container", passthrough=True, show_tail=False) as stage_report:
            stage_report.run(
                _checksum_container_command(
                    kern,
                    package=package,
                    image=image,
                    offline=offline,
                    stage=stage,
                    downloads=downloads,
                ),
                env=kern_env.kern_environment(),
            )
        generated = stage / "alpine/aports" / package / "APKBUILD"
        if generated.is_symlink() or not generated.is_file():
            fail(f"abuild checksum did not leave a regular APKBUILD: {generated}")
        generated_apkbuild = generated.read_bytes()
    if _aport_checksum_snapshot(package).recipe != before_snapshot.recipe:
        fail(f"Alpine aport source closure changed while checksums were generated: {package}")
    if apkbuild.is_symlink() or not apkbuild.is_file() or apkbuild.read_bytes() != before_apkbuild:
        fail(f"canonical APKBUILD changed while checksums were generated: {apkbuild}")
    _checksum_block_only_changed(before_apkbuild, generated_apkbuild, path=apkbuild)
    replace_file_atomically(apkbuild, generated_apkbuild, mode)
    print(f"checksum {package}: OK", flush=True)
    reporter.finish()

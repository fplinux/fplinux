# SPDX-License-Identifier: GPL-2.0-only
"""Build one project-local C probe with the pinned ARM musl toolchain."""

from __future__ import annotations

import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.alpine.packages import _alpine_group_packages, alpine_sysroot_command
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.build.workflow import ensure_build_directory
from fplinux_cli.common import canonical_json_bytes, fail, relative_name, sha256_bytes
from fplinux_cli.data_paths import data_root
from fplinux_cli.dependencies.inputs import acquire_inputs, alpine_package_input, source_input
from fplinux_cli.environment import kern as environment_kern
from fplinux_cli.environment import runtime as environment_runtime
from fplinux_cli.manifests.source_catalog import acquire_source, resolve_source
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace import build_inputs as workspace_inputs
from fplinux_cli.workspace import capture as workspace_capture
from fplinux_cli.workspace import staging as workspace_staging

if TYPE_CHECKING:
    from fplinux_cli.environment.state import EnvironmentState


def _require_unlinked_path(root: Path, relative: str) -> Path:
    """Reject symlinks in every supplied path component, including a missing target."""
    path = root
    for component in PurePosixPath(relative).parts:
        path /= component
        if path.is_symlink():
            fail(f"probe path must not use a symlink: {relative}")
    return path


def resolve_probe_paths(
    source: str, output: str, *, root: Path, cache: Path | None = None
) -> tuple[Path, Path]:
    """Validate explicit repository paths before loading any build environment."""
    source_name = relative_name(source, field="probe source")
    source_path = _require_unlinked_path(root, source_name)
    cache = data_root(root) if cache is None else cache
    if source_path.is_relative_to(cache):
        fail("probe source must be outside the managed data directory")
    if source_path.suffix != ".c" or not source_path.is_file():
        fail(f"probe source must name a regular .c file: {source_name}")
    tools_root = cache / "tools"
    requested_output = Path(output)
    output_path = (
        requested_output
        if requested_output.is_absolute()
        else root / relative_name(output, field="probe output")
    )
    if ".." in output_path.parts or not output_path.is_relative_to(tools_root):
        fail("probe output must be inside the selected managed tools directory")
    relative_output = output_path.relative_to(tools_root)
    if not relative_output.parts or tools_root.is_symlink():
        fail("probe output must name a file inside the selected managed tools directory")
    output_path = _require_unlinked_path(tools_root, relative_output.as_posix())
    if output_path.exists() and not output_path.is_file():
        fail(f"probe output must name a regular file: {output}")
    for parent in output_path.parents:
        if parent == tools_root.parent:
            break
        if parent.exists() and not parent.is_dir():
            fail(f"probe output parent must be a directory: {parent}")
    if output_path == source_path:
        fail("probe output must differ from its source")
    return source_path, output_path


def _container_command(
    kern: str,
    rootfs: Path,
    command: list[str],
    *,
    volumes: list[tuple[Path, str]],
    prepare_sysroot: bool = False,
) -> list[str]:
    """Keep probe preparation and compilation in the same offline pinned environment."""
    mounts = [
        argument
        for source, destination in volumes
        for argument in ("--volume", f"{source}:{destination}")
    ]
    return [
        kern,
        "box",
        environment_kern.kern_box_name("probe-build"),
        *environment_runtime.environment_arguments(rootfs),
        "--network",
        "none",
        "--read-only",
        "--no-uid-range",
        *(["--privileged"] if prepare_sysroot else []),
        "--tmpfs",
        "/tmp:256m",  # noqa: S108 -- disposable compiler scratch space.
        *mounts,
        "--env",
        "LC_ALL=C",
        "--env",
        f"SOURCE_DATE_EPOCH={SOURCE_DATE_EPOCH}",
        "--init",
        "--quiet",
        "--",
        *command,
    ]


def _prepare_sysroot(
    kern: str,
    rootfs: Path,
    state: EnvironmentState,
    reporter: RunReporter,
    *,
    cache: Path | None = None,
) -> Path:
    lock = alpine_lock.load_alpine_lock()
    records = alpine_lock.package_records(lock)
    identity = sha256_bytes(
        canonical_json_bytes(
            {
                "arch": lock["arch"],
                "packages": [records[name] for name in lock["sysroot"]["packages"]],
                "minirootfs": lock["minirootfs"],
                "environment_recipe": state.recipe,
                "environment_generation": state.generation,
            }
        )
    )
    cache = data_root(common.ROOT) if cache is None else cache
    sysroots = ensure_build_directory(cache / "probe-build")
    sysroot = sysroots / identity
    if sysroot.is_symlink() or (sysroot.exists() and not sysroot.is_dir()):
        fail(f"invalid probe sysroot cache directory: {sysroot}")
    if sysroot.is_dir():
        print("probe-build: reusing prepared sysroot", flush=True)
        return sysroot

    with reporter.stage("sysroot-inputs"):
        selected_inputs = [
            source_input(resolve_source(common.ROOT, lock["minirootfs"]["source_id"])),
            *(alpine_package_input(lock, records[name]) for name in lock["sysroot"]["packages"]),
        ]
        acquire_inputs(selected_inputs, cache, offline=True)
        packages = _alpine_group_packages(lock, records, "sysroot", cache=cache)
    downloads = cache / "downloads/alpine/packages"
    with tempfile.TemporaryDirectory(dir=sysroots, prefix=".prepare-") as temporary:
        prepared = Path(temporary) / "sysroot"
        prepared.mkdir()
        with reporter.stage("sysroot-keys"):
            keys = _prepare_keys(lock, Path(temporary), cache=cache)
        command = alpine_sysroot_command(
            lock,
            [Path("/packages") / package.name for package in packages],
            Path("/sysroot"),
            Path("/keys"),
        )
        with reporter.stage("sysroot") as stage:
            stage.run(
                _container_command(
                    kern,
                    rootfs,
                    command,
                    volumes=[
                        (downloads, "/packages:ro"),
                        (keys, "/keys:ro"),
                        (prepared, "/sysroot"),
                    ],
                    prepare_sysroot=True,
                ),
                cwd=common.ROOT,
                env=environment_kern.kern_environment(cache=cache),
                timeout=5 * 60,
            )
        prepared.rename(sysroot)
    return sysroot


def _prepare_keys(lock: dict[str, Any], destination: Path, *, cache: Path | None = None) -> Path:
    """Use the target release's signing keys, as the normal rootfs build does."""
    record = lock["minirootfs"]
    cache = data_root(common.ROOT) if cache is None else cache
    archive = acquire_source(common.ROOT, record["source_id"], cache)
    if archive.stat().st_size != record["bytes"]:
        fail("locked Alpine minirootfs size does not match its downloaded bytes")
    with tarfile.open(archive, "r:gz") as bundle:
        members = [
            member
            for member in bundle.getmembers()
            if member.isfile()
            and PurePosixPath(member.name).parent == PurePosixPath("etc/apk/keys")
        ]
        bundle.extractall(destination, members=members, filter="data")
    return destination / "etc/apk/keys"


def _compile_probe(  # noqa: PLR0913 -- explicit compiler inputs and reporting boundary.
    source: Path,
    output: Path,
    sysroot: Path,
    *,
    kern: str,
    rootfs: Path,
    reporter: RunReporter,
    cache: Path | None = None,
) -> None:
    source_relative = source.relative_to(common.ROOT)
    cache = data_root(common.ROOT) if cache is None else cache
    source_files = {source_relative.as_posix(): source}
    workspace_inputs.add_source_path(
        source_files, common.ROOT / "include", selected_data_root=cache
    )
    for header in sorted(source.parent.glob("*.h")):
        if header.is_file():
            source_files[header.relative_to(common.ROOT).as_posix()] = header
    snapshot = workspace_capture.workspace_snapshot(list(source_files.items()))
    workspace = workspace_staging.stage_workspace_snapshot(snapshot, selected_data_root=cache)
    temporary: Path | None = None
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            dir=output.parent, prefix=f".{output.name}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
        command = [
            "clang",
            "--target=armv7-alpine-linux-musleabihf",
            "--sysroot=/sysroot",
            "--gcc-toolchain=/sysroot/usr",
            "-march=armv7-a",
            "-mfpu=vfpv3-d16",
            "-mfloat-abi=hard",
            "-fuse-ld=lld",
            "-static",
            "-O2",
            "-I/workspace/include",
            "-I/workspace/include/fplinux",
            "-o",
            f"/output/{temporary.name}",
            f"/workspace/{source_relative.as_posix()}",
        ]
        with reporter.stage("compile") as stage:
            stage.run(
                _container_command(
                    kern,
                    rootfs,
                    command,
                    volumes=[
                        (workspace, "/workspace:ro"),
                        (sysroot, "/sysroot:ro"),
                        (output.parent, "/output"),
                    ],
                ),
                cwd=common.ROOT,
                env=environment_kern.kern_environment(cache=cache),
                timeout=5 * 60,
            )
        temporary.chmod(0o755)
        temporary.replace(output)
        temporary = None
    finally:
        try:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        finally:
            workspace_staging.discard_staged_workspace_snapshot(
                snapshot, workspace, selected_data_root=cache
            )


def build_probe(source: str, *, output: str) -> None:
    """Publish one static ARMv7 probe only after the pinned compiler succeeds."""
    cache = data_root(common.ROOT)
    source_path, output_path = resolve_probe_paths(source, output, root=common.ROOT, cache=cache)
    kern, rootfs, state = environment_runtime.require_environment(cache=cache)
    reporter = RunReporter.create(
        "probe-build", target=None, verbose=False, selected_data_root=cache
    )
    sysroot = _prepare_sysroot(kern, rootfs, state, reporter, cache=cache)
    _compile_probe(
        source_path, output_path, sysroot, kern=kern, rootfs=rootfs, reporter=reporter, cache=cache
    )
    print(f"probe-build: {output}", flush=True)
    reporter.finish()

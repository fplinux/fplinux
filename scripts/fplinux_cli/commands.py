# SPDX-License-Identifier: GPL-2.0-only
"""Target commands dispatched through repository metadata."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from . import alpine_state
from .bundle_state import (
    BundleStateError,
    CurrentBundle,
    resolve_current_bundle,
)
from .common import (
    ROOT,
    ZIP_TIMESTAMP,
    fail,
    payload_digest,
    sha256_bytes,
    sha256_file,
)
from .config import (
    container_image_recipe_digest,
    load_container_lock,
    load_platform,
    load_release,
    load_target,
    verified_runtime_digest,
)
from .container import image_ready, require_podman, setup
from .output import RunReporter, silence_broken_pipe
from .workspace import (
    WorkspaceSnapshot,
    stage_workspace_snapshot,
    target_workspace_snapshot,
)

CANDIDATE_NOTICE = b"""HARDWARE QUALIFICATION CANDIDATE - DO NOT PUBLISH

This archive is for physical device qualification and testing only.
Candidate packaging does not assert release qualification.
"""


@dataclass(frozen=True)
class BuildIdentity:
    """Exact host-visible inputs that authorize bundle reuse."""

    workspace_digest: str
    container_image_recipe: str
    apk_signing_key: str


PACKAGE_DOCUMENTS = {
    "LICENSE": ROOT / "LICENSE",
    "licenses/musl/COPYRIGHT": ROOT / "THIRD_PARTY_LICENSES/musl/COPYRIGHT",
}


def _bundle_manifest(bundle: CurrentBundle) -> dict[str, Any]:
    """Decode manifest bytes already validated by the immutable bundle resolver."""
    manifest = json.loads(bundle.manifest_bytes)
    if not isinstance(manifest, dict):
        message = "build manifest root must be an object"
        raise BundleStateError(message)
    return manifest


def _resolve_target_bundle(
    target: str,
    config: dict[str, Any],
) -> tuple[CurrentBundle, dict[str, Any]]:
    """Resolve the current bundle pointer exactly once."""
    try:
        bundle = resolve_current_bundle(
            ROOT / ".cache/out",
            target,
            config["profile"],
        )
        return bundle, _bundle_manifest(bundle)
    except (BundleStateError, OSError, UnicodeDecodeError, ValueError) as error:
        fail(
            f"current build is missing or invalid; rebuild it: ./fplinux build {target} ({error})"
        )


def _manifest_matches_identity(manifest: dict[str, Any], identity: BuildIdentity | None) -> bool:
    """Return whether a manifest matches one exact host-visible build identity."""
    return identity is not None and all(
        manifest.get(field) == value
        for field, value in (
            ("workspace_digest", identity.workspace_digest),
            ("container_image_recipe", identity.container_image_recipe),
            ("apk_signing_key", identity.apk_signing_key),
        )
    )


def _matching_target_bundle(
    target: str,
    config: dict[str, Any],
    identity: BuildIdentity | None,
    image_relative: str,
) -> tuple[CurrentBundle, dict[str, Any]] | None:
    """Return only a fully valid current generation for the exact causal inputs."""
    try:
        bundle = resolve_current_bundle(
            ROOT / ".cache/out",
            target,
            config["profile"],
        )
        manifest = _bundle_manifest(bundle)
    except (BundleStateError, OSError, UnicodeDecodeError, ValueError):
        return None
    if not _manifest_matches_identity(manifest, identity):
        return None
    files = manifest.get("files")
    record = files.get(image_relative) if isinstance(files, dict) else None
    expected = record.get("sha256") if isinstance(record, dict) else None
    image = bundle.path / image_relative
    if not isinstance(expected, str) or not image.is_file() or sha256_file(image) != expected:
        return None
    return bundle, manifest


def _console_client(bundle: CurrentBundle) -> Path:
    client = bundle.path / "host/fplinux-usb-console"
    if client.is_symlink() or not client.is_file():
        fail(f"current bundle has no valid USB console client: {client}")
    return client


def _console_connection(config: dict[str, Any]) -> list[str]:
    console = config["runtime"]["usb"]["linux_console"]
    return [
        "--vid",
        f"{console['vendor_id']:04x}",
        "--pid",
        f"{console['product_id']:04x}",
        "--wait",
        str(console["wait_seconds"]),
    ]


def console_target(
    target: str,
    *,
    keyboard: str | None,
    exec_command: str | None,
    upload: list[str] | None,
    pull: list[str] | None,
) -> None:
    """Run the built target's USB console client."""
    config = load_target(target)
    bundle, _manifest = _resolve_target_bundle(target, config)
    client = _console_client(bundle)
    arguments = [
        str(client),
        *_console_connection(config),
        "--interface",
        "1" if keyboard is not None else "0",
    ]
    if keyboard is not None:
        arguments.extend(["--keyboard", keyboard])
    elif exec_command is not None:
        arguments.extend(["--exec", exec_command])
    elif upload is not None:
        arguments.extend(["--upload", *upload])
    elif pull is not None:
        arguments.extend(["--pull", *pull])
    os.execv(client, arguments)


def verify_booted(target: str) -> None:
    """Compare the running kernel identity with the current bundle."""
    config = load_target(target)
    bundle, manifest = _resolve_target_bundle(target, config)
    snapshot = target_workspace_snapshot(target)
    image_recipe = container_image_recipe_digest()
    identity = _build_identity(snapshot, image_recipe, ROOT / ".cache")
    if not _manifest_matches_identity(manifest, identity):
        fail(f"build output is stale; rebuild it: ./fplinux build {target}")
    device_identity = manifest.get("device_identity")
    if (
        not isinstance(device_identity, str)
        or len(device_identity) != 64
        or any(character not in "0123456789abcdef" for character in device_identity)
    ):
        fail(f"build output is stale; rebuild it: ./fplinux build {target}")
    expected_kernel_suffix = f"-fplinux-{device_identity[:16]}"
    client = _console_client(bundle)
    result = subprocess.run(
        [
            str(client),
            *_console_connection(config),
            "--interface",
            "0",
            "--exec",
            "uname -r",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.strip().splitlines()
        raise SystemExit(
            f"verify: console client failed with exit status {result.returncode}\n  "
            + (detail[-1] if detail else "the console client gave no diagnostic")
        )
    actual = result.stdout.splitlines()
    if not actual:
        detail = result.stderr.strip().splitlines()
        raise SystemExit(
            "verify: no kernel identity came back from the phone\n  "
            + (detail[-1] if detail else "the console client said nothing")
        )
    if len(actual) != 1 or not actual[0].endswith(expected_kernel_suffix):
        raise SystemExit(
            "verify: the phone is running a different build\n"
            f"  phone:  {result.stdout.strip()}\n"
            f"  bundle: kernel *{expected_kernel_suffix}\n"
            "Load the current image before trusting anything you measure."
        )
    print(f"verify: the phone runs the current build ({device_identity[:16]})")


def _build_identity(
    snapshot: WorkspaceSnapshot, image_recipe: str, cache: Path
) -> BuildIdentity | None:
    """Read the exact host-visible inputs without creating signing state."""
    try:
        signing_key = alpine_state.signing_key_identity(cache)
    except SystemExit:
        return None
    return BuildIdentity(snapshot.recipe, image_recipe, signing_key)


def _ensure_build_directory(path: Path) -> Path:
    """Create one exact build mount root without accepting a symlink or file."""
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        fail(f"invalid build cache directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or not path.is_dir():
        fail(f"invalid build cache directory: {path}")
    return path


def _build_container_command(  # noqa: PLR0913
    podman: str,
    *,
    target: str,
    jobs: int,
    platform: str,
    image: str,
    snapshot: WorkspaceSnapshot,
    workspace: Path,
    downloads: Path,
    apk_signing: Path,
    apks: Path,
    rootfs: Path,
    linux: Path,
    output: Path,
    logs: Path,
    log_environment: dict[str, str],
    image_recipe: str,
) -> list[str]:
    """Return the exact target-build argv with only narrow explicit mounts."""
    log_arguments = [
        argument
        for key, value in log_environment.items()
        for argument in ("--env", f"{key}={value}")
    ]
    return [
        podman,
        "run",
        "--rm",
        "--platform",
        platform,
        "--userns=keep-id:uid=0,gid=0",
        "--read-only",
        "--tmpfs",
        "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs.
        "--volume",
        f"{downloads}:/cache/downloads:rw,Z",
        "--volume",
        f"{apk_signing}:/cache/apk-signing:rw,Z",
        "--volume",
        f"{apks}:/cache/apks:rw,Z",
        "--volume",
        f"{rootfs}:/cache/rootfs:rw,Z",
        "--volume",
        f"{linux}:/cache/linux:rw,Z",
        "--volume",
        f"{output}:/out:rw,Z",
        "--volume",
        f"{logs}:/logs:rw,Z",
        "--volume",
        f"{workspace}:/workspace:ro,Z",
        *log_arguments,
        "--env",
        "HOME=/tmp/fplinux-home",
        "--env",
        "PYTHONPATH=/workspace/scripts",
        "--env",
        f"FPLINUX_CONTAINER_IMAGE_RECIPE={image_recipe}",
        "--env",
        f"FPLINUX_WORKSPACE_DIGEST={snapshot.recipe}",
        image,
        "python3",
        "-m",
        "fplinux_cli.builder",
        "--target",
        target,
        "--jobs",
        str(jobs),
    ]


def _print_build_result(
    target: str,
    bundle: CurrentBundle,
    release: dict[str, Any],
    *,
    cached: bool,
) -> None:
    image = bundle.path / release["image"]
    if image.is_symlink() or not image.is_file():
        fail(f"current bundle image is missing or invalid: {image}")
    suffix = " (cached)" if cached else ""
    try:
        print(f"build {target}: OK{suffix}", flush=True)
        print(f"output: {bundle.path.relative_to(ROOT)}", flush=True)
        print(f"ramboot.bin SHA256: {sha256_file(image)}", flush=True)
    except BrokenPipeError:
        silence_broken_pipe(sys.stdout)


def build(target: str, jobs: int, *, verbose: bool = False) -> None:
    if jobs < 1:
        fail("--jobs must be positive")
    target_config = load_target(target)
    release = load_release(target, target_config)
    snapshot = target_workspace_snapshot(target)
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    cache = ROOT / ".cache"
    identity = _build_identity(snapshot, image_recipe, cache)
    current = _matching_target_bundle(
        target,
        target_config,
        identity,
        release["image"],
    )
    if current is not None:
        bundle, _manifest = current
        reporter = RunReporter.create("build", target=target, verbose=verbose)
        _print_build_result(target, bundle, release, cached=True)
        reporter.finish()
        return

    reporter = RunReporter.create("build", target=target, verbose=verbose)
    podman = require_podman()
    lock = container_lock["oci"]
    if not image_ready(podman, lock["image"], image_recipe=image_recipe):
        setup(reporter=reporter, lock=container_lock, image_recipe=image_recipe)
    apk_signing = _ensure_build_directory(cache / "apk-signing")
    downloads = _ensure_build_directory(cache / "downloads")
    apks = _ensure_build_directory(cache / "apks")
    rootfs = _ensure_build_directory(cache / "rootfs")
    linux = _ensure_build_directory(cache / "linux")
    output = _ensure_build_directory(cache / "out")
    with reporter.stage("workspace"):
        workspace = stage_workspace_snapshot(snapshot)

    log_environment = reporter.container_environment("/logs")
    with reporter.stage("container", passthrough=True, show_tail=False) as stage:
        stage.run(
            _build_container_command(
                podman,
                target=target,
                jobs=jobs,
                platform=lock["platform"],
                image=lock["image"],
                snapshot=snapshot,
                workspace=workspace,
                downloads=downloads,
                apk_signing=apk_signing,
                apks=apks,
                rootfs=rootfs,
                linux=linux,
                output=output,
                logs=reporter.root,
                log_environment=log_environment,
                image_recipe=image_recipe,
            )
        )
    identity = _build_identity(snapshot, image_recipe, cache)
    current = _matching_target_bundle(
        target,
        target_config,
        identity,
        release["image"],
    )
    if current is None:
        fail("build completed without publishing an exact valid current bundle")
    bundle, _manifest = current
    _print_build_result(target, bundle, release, cached=False)
    reporter.finish()


def target_archive_file(target: str, relative: str) -> tuple[str, Path]:
    source_name = PurePosixPath(relative)
    try:
        archive_name = source_name.relative_to("release").as_posix()
    except ValueError:
        fail(f"target package file must be below release/: {relative}")
    if not archive_name or archive_name == ".":
        fail(f"target package file has no archive name: {relative}")
    source = ROOT / "targets" / target / relative
    if source.is_symlink() or not source.is_file():
        fail(f"target package file is missing or invalid: {source}")
    return archive_name, source


def load_release_manifest(target: str, config: dict[str, Any]) -> dict[str, Any]:
    """Resolve release documents and fixed platform executable roles."""
    manifest = load_release(target, config)
    image = manifest["image"]
    bundle_files = manifest["bundle_files"]
    runtime_files = manifest["runtime_files"]
    documents = manifest["documents"]
    archive_names = set(bundle_files)
    for relative in documents:
        archive_name, _source = target_archive_file(target, relative)
        if archive_name in archive_names:
            fail(f"duplicate release archive path: {archive_name}")
        archive_names.add(archive_name)

    platform = load_platform(config["platform"])
    executables = {
        "runner/run.py",
        *(f"host/{tool['name']}" for tool in platform["host"]["tools"]),
    }
    required_runtime = {
        image,
        "runtime-manifest.json",
        "runner/platform_adapter.py",
        *config["runtime"]["assets"].values(),
        *executables,
    }
    if not required_runtime.issubset(runtime_files):
        fail("release runtime files omit required runtime inputs")
    return {
        "image": image,
        "bundle_files": bundle_files,
        "runtime_files": runtime_files,
        "documents": documents,
        "executables": executables,
    }


def add_target_files(files: dict[str, bytes], target: str, relative_names: list[str]) -> None:
    for relative in relative_names:
        archive_name, source = target_archive_file(target, relative)
        files[archive_name] = source.read_bytes()


def package_target(target: str, *, candidate: bool = False) -> None:
    config = load_target(target)
    release = load_release_manifest(target, config)
    profile = config["profile"]
    bundle, manifest = _resolve_target_bundle(target, config)
    snapshot = target_workspace_snapshot(target)
    image_recipe = container_image_recipe_digest()
    identity = _build_identity(snapshot, image_recipe, ROOT / ".cache")
    files_table = manifest.get("files")
    if (
        not _manifest_matches_identity(manifest, identity)
        or not isinstance(files_table, dict)
        or not set(release["bundle_files"]).issubset(files_table)
    ):
        fail(f"build output is stale; rebuild it: ./fplinux build {target}")

    files: dict[str, bytes] = {}
    for relative in release["bundle_files"]:
        source = bundle.path / relative
        if source.is_symlink() or not source.is_file():
            fail(f"release input is missing or invalid: {source}")
        data = source.read_bytes()
        record = files_table[relative]
        if (
            not isinstance(record, dict)
            or record.get("sha256") != sha256_bytes(data)
            or record.get("mode") != (source.stat().st_mode & 0o777)
        ):
            fail(f"release input differs from its successful build manifest: {source}")
        files[relative] = data

    runtime_payload = {relative: files[relative] for relative in release["runtime_files"]}
    runtime_digest = payload_digest(runtime_payload, release["executables"])
    files["BUILD-MANIFEST.json"] = bundle.manifest_bytes
    verified_digest = verified_runtime_digest(target)
    if not candidate and verified_digest != runtime_digest:
        fail(
            "this runtime closure is not hardware-qualified for release; "
            f"use --candidate for device testing (runtime SHA256 {runtime_digest})"
        )

    add_target_files(files, target, release["documents"])
    if candidate:
        files["CANDIDATE-NOTICE.txt"] = CANDIDATE_NOTICE
    for archive_name, source in PACKAGE_DOCUMENTS.items():
        if source.is_symlink() or not source.is_file():
            fail(f"release document is missing or invalid: {source}")
        files[archive_name] = source.read_bytes()
    checksums = "".join(f"{sha256_bytes(files[name])}  {name}\n" for name in sorted(files))
    files["SHA256SUMS"] = checksums.encode()
    content_digest = payload_digest(files, release["executables"])

    qualifier = "candidate" if candidate else "release"
    stem = f"{config['release_slug']}-{profile}-{qualifier}-linux-x86_64-{content_digest[:16]}"
    destination = ROOT / ".cache/out" / ("candidates" if candidate else "releases")
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / f"{stem}.zip"
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination,
            prefix=f".{stem}.",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as output:
            for relative in sorted(files):
                info = zipfile.ZipInfo(f"{stem}/{relative}", ZIP_TIMESTAMP)
                info.create_system = 3
                mode = 0o100755 if relative in release["executables"] else 0o100644
                info.external_attr = mode << 16
                info.compress_type = zipfile.ZIP_STORED
                output.writestr(info, files[relative])
        temporary.chmod(0o644)
        if archive.exists():
            if sha256_file(archive) != sha256_file(temporary):
                fail(f"package name collision with different bytes: {archive}")
            temporary.unlink()
            temporary = None
            archive.chmod(0o644)
        else:
            temporary.replace(archive)
            temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

    image_digest = sha256_bytes(files[release["image"]])
    print(f"FPLinux {qualifier} package: {archive.relative_to(ROOT)}")
    print(f"Archive SHA256: {sha256_file(archive)}")
    print(f"Runtime closure SHA256: {runtime_digest}")
    print(f"ramboot.bin SHA256: {image_digest}")


def run_target(target: str) -> None:
    """Run the fixed shared runner from a successful target bundle."""
    config = load_target(target)
    bundle, _manifest = _resolve_target_bundle(target, config)
    runner = bundle.path / "runner/run.py"
    if runner.is_symlink() or not runner.is_file():
        fail(f"current bundle has no valid runner: {runner}")
    os.execv(os.fsencode(runner), [os.fsencode(runner)])

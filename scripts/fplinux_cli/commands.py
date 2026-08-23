# SPDX-License-Identifier: GPL-2.0-only
"""Target commands dispatched through repository metadata."""

from __future__ import annotations

import importlib.util
import json
import os
import shlex
import sys
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from types import ModuleType

from . import alpine_state
from .alpine_builder import SOURCE_DATE_EPOCH, materialize_aport_sources
from .bundle_state import (
    BundleStateError,
    CurrentBundle,
    discard_superseded_bundle_generations,
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
    container_image_reference,
    load_container_lock,
    load_platform,
    load_release,
    load_target,
    verified_runtime_digest,
)
from .container import image_identifier, image_ready, require_podman, setup
from .output import RunReporter, silence_broken_pipe
from .workspace import (
    WorkspaceSnapshot,
    add_source_path,
    stage_workspace_snapshot,
    target_workspace_snapshot,
    workspace_snapshot,
)

CANDIDATE_NOTICE = b"""HARDWARE QUALIFICATION CANDIDATE - DO NOT PUBLISH

This archive is for physical device qualification and testing only.
Candidate packaging does not assert release qualification.
"""
SSH_HELPER_PATH = "runner/ssh_transport.py"


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
) -> tuple[CurrentBundle, dict[str, Any]]:
    """Resolve the current bundle pointer exactly once."""
    try:
        bundle = resolve_current_bundle(
            ROOT / ".cache/out",
            target,
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
    identity: BuildIdentity | None,
    image_relative: str,
) -> tuple[CurrentBundle, dict[str, Any]] | None:
    """Return only a fully valid current generation for the exact causal inputs."""
    try:
        bundle = resolve_current_bundle(
            ROOT / ".cache/out",
            target,
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


def _load_bundle_ssh_helper(
    bundle: CurrentBundle,
    manifest: dict[str, Any],
) -> ModuleType:
    """Load only the SSH helper hashed by the selected immutable generation."""
    path = bundle.path / SSH_HELPER_PATH
    files = manifest.get("files")
    record = files.get(SSH_HELPER_PATH) if isinstance(files, dict) else None
    expected = record.get("sha256") if isinstance(record, dict) else None
    if (
        not isinstance(expected, str)
        or path.is_symlink()
        or not path.is_file()
        or sha256_file(path) != expected
    ):
        fail(f"current bundle has no valid SSH transport helper: {path}")
    name = f"fplinux_bundle_ssh_transport_{bundle.generation}"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"current bundle SSH transport helper cannot be loaded: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    required = {
        "load_bundle_context",
        "load_current_session",
        "reacquire_bound_session",
        "run_remote",
        "upload",
        "pull",
        "open_shell",
    }
    if any(not callable(getattr(module, name, None)) for name in required):
        fail("current bundle SSH transport helper has an incompatible API")
    return module


def _current_ssh_session(
    bundle: CurrentBundle,
    manifest: dict[str, Any],
    target: str,
) -> tuple[ModuleType, dict[str, Any]]:
    """Load and reacquire only a session created by this exact selected bundle."""
    ssh = _load_bundle_ssh_helper(bundle, manifest)
    runtime, identity = ssh.load_bundle_context(bundle.path)
    if runtime.get("target") != target or identity.get("bundle_generation") != bundle.generation:
        fail("current bundle SSH identity disagrees with the selected generation")
    session = ssh.load_current_session(target, identity)
    return ssh, ssh.reacquire_bound_session(session)


def _keyboard_client(bundle: CurrentBundle) -> Path:
    client = bundle.path / "host/fplinux-usb-keyboard"
    if client.is_symlink() or not client.is_file():
        fail(f"current bundle has no valid USB keyboard client: {client}")
    return client


def _keyboard_connection(config: dict[str, Any]) -> list[str]:
    gadget = config["runtime"]["usb"]["linux_gadget"]
    return [
        "--vid",
        f"{gadget['vendor_id']:04x}",
        "--pid",
        f"{gadget['product_id']:04x}",
        "--wait",
        str(gadget["wait_seconds"]),
    ]


def _keyboard_interface(config: dict[str, Any]) -> str:
    """Return the runtime-declared generic-serial keyboard interface."""
    return str(config["runtime"]["usb"]["linux_gadget"]["keyboard_interface"])


def console_target(
    target: str,
    *,
    keyboard: str | None,
    exec_command: str | None,
    upload: list[str] | None,
    pull: list[str] | None,
) -> None:
    """Open the SSH session, or forward one evdev keyboard over USB."""
    config = load_target(target)
    bundle, manifest = _resolve_target_bundle(target)
    if keyboard is None:
        ssh_transport, session = _current_ssh_session(bundle, manifest, target)
        if exec_command is not None:
            result = ssh_transport.run_remote(session, exec_command)
            if result.returncode:
                raise SystemExit(result.returncode)
            return
        if upload is not None:
            ssh_transport.upload(session, upload[0], upload[1])
            return
        if pull is not None:
            ssh_transport.pull(session, pull[0], pull[1])
            return
        ssh_transport.open_shell(session)
        return
    client = _keyboard_client(bundle)
    arguments = [
        str(client),
        *_keyboard_connection(config),
        "--interface",
        _keyboard_interface(config),
        "--keyboard",
        keyboard,
    ]
    os.execv(client, arguments)


def verify_booted(target: str) -> None:
    """Compare the running kernel identity with the current bundle."""
    bundle, manifest = _resolve_target_bundle(target)
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
    ssh_transport, session = _current_ssh_session(bundle, manifest, target)
    result = ssh_transport.run_remote(session, "uname -r", capture_output=True)
    transport_name = "SSH transport"
    if result.returncode:
        detail = result.stderr.strip().splitlines()
        raise SystemExit(
            f"verify: {transport_name} failed with exit status {result.returncode}\n  "
            + (detail[-1] if detail else f"the {transport_name} gave no diagnostic")
        )
    actual = result.stdout.splitlines()
    if not actual:
        detail = result.stderr.strip().splitlines()
        raise SystemExit(
            "verify: no kernel identity came back from the phone\n  "
            + (detail[-1] if detail else f"the {transport_name} said nothing")
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


def _canonical_aport(package: str) -> Path:
    """Return one regular canonical aport selected by a package identifier."""
    if alpine_state.PACKAGE_ID.fullmatch(package) is None:
        fail(f"invalid Alpine package identifier: {package}")
    aport = ROOT / "alpine/aports" / package
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
    for source in alpine_state.shared_aport_sources(package, root=ROOT):
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


def _replace_file_atomically(path: Path, contents: bytes, mode: int) -> None:
    """Publish one verified regular file without exposing a partial write."""
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=path.parent,
            prefix=f".{path.name}.",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(mode)
        temporary.replace(path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _checksum_container_command(  # noqa: PLR0913
    podman: str,
    *,
    package: str,
    platform: str,
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
        podman,
        "run",
        "--rm",
        *(["--network=none"] if offline else []),
        "--platform",
        platform,
        "--userns=keep-id:uid=0,gid=0",
        "--read-only",
        "--tmpfs",
        "/tmp:rw,nosuid,nodev",  # noqa: S108 -- container tmpfs.
        "--volume",
        f"{downloads}:/cache/downloads:rw,Z",
        "--volume",
        f"{stage}:/workspace:rw,Z",
        "--workdir",
        stage_aport,
        "--env",
        "HOME=/tmp/fplinux-home",
        image,
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
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    podman = require_podman()
    lock = container_lock["oci"]
    image = container_image_reference(container_lock, image_recipe)
    reporter = RunReporter.create("checksum", target=package, verbose=False)
    if not image_ready(podman, image, image_recipe=image_recipe):
        if offline:
            fail(
                "offline checksum requires the current pinned OCI image; "
                "run ./fplinux setup online first"
            )
        setup(reporter=reporter, lock=container_lock, image_recipe=image_recipe)
    image_identity = image_identifier(podman, image)
    if image_identity is None:
        fail("checksum requires an immutable current build image")
    cache = ROOT / ".cache"
    downloads = _ensure_build_directory(cache / "downloads")
    with reporter.stage("workspace"):
        snapshot_root = stage_workspace_snapshot(before_snapshot)
    checksum_root = _ensure_build_directory(cache / "checksum")
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
                    podman,
                    package=package,
                    platform=lock["platform"],
                    image=image_identity,
                    offline=offline,
                    stage=stage,
                    downloads=downloads,
                )
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
    _replace_file_atomically(apkbuild, generated_apkbuild, mode)
    print(f"checksum {package}: OK", flush=True)
    reporter.finish()


def _build_container_command(  # noqa: PLR0913
    podman: str,
    *,
    target: str,
    jobs: int,
    platform: str,
    image: str,
    offline: bool,
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
        *(["--network=none"] if offline else []),
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


def build(target: str, jobs: int, *, verbose: bool = False, offline: bool = False) -> None:
    if jobs < 1:
        fail("--jobs must be positive")
    release = load_release(target)
    snapshot = target_workspace_snapshot(target)
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    cache = ROOT / ".cache"
    identity = _build_identity(snapshot, image_recipe, cache)
    current = _matching_target_bundle(
        target,
        identity,
        release["image"],
    )
    if current is not None:
        bundle, _manifest = current
        discard_superseded_bundle_generations(
            cache / "out",
            target,
            bundle,
        )
        reporter = RunReporter.create("build", target=target, verbose=verbose)
        _print_build_result(target, bundle, release, cached=True)
        reporter.finish()
        return

    reporter = RunReporter.create("build", target=target, verbose=verbose)
    podman = require_podman()
    lock = container_lock["oci"]
    image = container_image_reference(container_lock, image_recipe)
    if not image_ready(podman, image, image_recipe=image_recipe):
        if offline:
            fail(
                "offline build requires the current pinned OCI image; "
                "run ./fplinux setup online first"
            )
        setup(reporter=reporter, lock=container_lock, image_recipe=image_recipe)
    image_identity = image_identifier(podman, image)
    if image_identity is None:
        fail("build requires an immutable current build image")
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
                image=image_identity,
                offline=offline,
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
        identity,
        release["image"],
    )
    if current is None:
        fail("build completed without publishing an exact valid current bundle")
    bundle, _manifest = current
    discard_superseded_bundle_generations(
        output,
        target,
        bundle,
    )
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
    manifest = load_release(target)
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
    required_runtime.add(SSH_HELPER_PATH)
    if not required_runtime.issubset(runtime_files):
        fail("release runtime files omit required runtime inputs")
    qualification_files = [
        *runtime_files,
        *(
            relative
            for relative in bundle_files
            if len(PurePosixPath(relative).parts) == 2
            and PurePosixPath(relative).parts[0] == "apks"
            and PurePosixPath(relative).suffix == ".apk"
            and relative not in runtime_files
        ),
    ]
    return {
        "image": image,
        "bundle_files": bundle_files,
        "runtime_files": runtime_files,
        "qualification_files": qualification_files,
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
    bundle, manifest = _resolve_target_bundle(target)
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

    qualification_payload = {
        relative: files[relative] for relative in release["qualification_files"]
    }
    qualification_digest = payload_digest(qualification_payload, release["executables"])
    files["build-manifest.json"] = bundle.manifest_bytes
    verified_digest = verified_runtime_digest(target)
    if not candidate and verified_digest != qualification_digest:
        fail(
            "this executable payload is not hardware-qualified for release; "
            "use --candidate for device testing "
            f"(qualification SHA256 {qualification_digest})"
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
    stem = f"{config['release_slug']}-{qualifier}-linux-x86_64-{content_digest[:16]}"
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
    print(f"Qualification payload SHA256: {qualification_digest}")
    print(f"ramboot.bin SHA256: {image_digest}")


def run_target(target: str) -> None:
    """Run the fixed shared runner from a successful target bundle."""
    bundle, _manifest = _resolve_target_bundle(target)
    runner = bundle.path / "runner/run.py"
    if runner.is_symlink() or not runner.is_file():
        fail(f"current bundle has no valid runner: {runner}")
    os.execv(os.fsencode(runner), [os.fsencode(runner)])

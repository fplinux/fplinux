# SPDX-License-Identifier: GPL-2.0-only
"""Package a selected build for standalone use."""

from __future__ import annotations

import json
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any

from fplinux_cli import common
from fplinux_cli.alpine.selection import bundle_packages, selected_packages
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.build import reuse as cli_bundles
from fplinux_cli.common import (
    ZIP_TIMESTAMP,
    canonical_json_bytes,
    fail,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.environment import definition
from fplinux_cli.environment import state as environment_states
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.packaging.autonomous import autonomous_payload
from fplinux_cli.packaging.documentation import offline_documentation
from fplinux_cli.packaging.hosts import runtime_hosts
from fplinux_cli.packaging.readme import append_source_companion, ordinary_readme
from fplinux_cli.runtime import bundle_session as runtime_bundle_session
from fplinux_cli.runtime.windows_tools import WINDOWS_TOOLS
from fplinux_cli.workspace import build_inputs as workspaces

SHARED_PACKAGE_FILES = {
    "LICENSE": "LICENSE",
    "licenses/musl/COPYRIGHT": "third-party-licenses/musl/COPYRIGHT",
}


def distribution_record(archive: Path, manifest: dict[str, Any], host: str) -> dict[str, str]:
    """Bind a final runtime archive to its phone and exact kernel identity."""
    return {
        "archive": archive.name,
        "target": manifest["target"],
        "host": host,
        "identity": manifest["device_identity"],
        "sha256": sha256_file(archive),
    }


def finalize_checksums(files: dict[str, bytes]) -> None:
    """Replace the complete member checksum list after the payload is final."""
    files["SHA256SUMS"] = "".join(
        f"{sha256_bytes(files[name])}  {name}\n" for name in sorted(files) if name != "SHA256SUMS"
    ).encode()


def finalize_runtime(
    files: dict[str, bytes], manifest: dict[str, Any], executables: set[str]
) -> None:
    """Project the final runtime members into its manifest and checksum inventory."""
    files.pop("build-manifest.json", None)
    files.pop("SHA256SUMS", None)
    files["build-manifest.json"] = _package_manifest(files, manifest, executables)
    finalize_checksums(files)


def target_archive_file(
    target: str,
    relative: str,
) -> tuple[str, Path]:
    """Map one target-owned release document into its stable archive path."""
    target_name = PurePosixPath(target)
    if (
        target_name.is_absolute()
        or len(target_name.parts) != 1
        or target_name.as_posix() != target
    ):
        fail(f"invalid target package name: {target}")
    source_name = PurePosixPath(relative)
    if (
        source_name.is_absolute()
        or ".." in source_name.parts
        or source_name.as_posix() != relative
    ):
        fail(f"target package file has an unsafe path: {relative}")
    try:
        archive_name = source_name.relative_to("release").as_posix()
    except ValueError:
        fail(f"target package file must be below release/: {relative}")
    if not archive_name or archive_name == ".":
        fail(f"target package file has no archive name: {relative}")
    target_root = common.ROOT / "targets" / target
    if target_root.is_symlink() or not target_root.is_dir():
        fail(f"target package root is missing or invalid: {target_root}")
    source = target_root
    for component in source_name.parts:
        source /= component
        if source.is_symlink():
            fail(f"target package file must not traverse a symlink: {source}")
    if not source.is_file():
        fail(f"target package file is missing or invalid: {source}")
    return archive_name, source


def load_release_manifest(target: str, config: dict[str, Any]) -> dict[str, Any]:
    """Resolve release documents and fixed platform executable roles."""
    platform = platforms.load_platform(config["platform"])
    rootfs_packages = selected_packages(platform, config, root=common.ROOT)
    selected_bundle_packages = bundle_packages(platform, config, rootfs_packages, root=common.ROOT)
    manifest = releases.load_release(target, bundle_packages=selected_bundle_packages)
    image = manifest["image"]
    bundle_files = manifest["bundle_files"]
    runtime_files = manifest["runtime_files"]
    if config.get("linux", {}).get("root", {}).get("kind") == "initramfs":
        bundle_files = [*bundle_files, "device-data.cpio"]
        runtime_files = [*runtime_files, "device-data.cpio"]
    documents = manifest["documents"]
    archive_names = set(bundle_files)
    if "README.txt" in archive_names:
        fail("release bundle files must not replace the generated README.txt")
    archive_names.add("README.txt")
    shared_collisions = archive_names & SHARED_PACKAGE_FILES.keys()
    if shared_collisions:
        fail(f"duplicate release archive path: {min(shared_collisions)}")
    archive_names.update(SHARED_PACKAGE_FILES)
    for relative in documents:
        archive_name, _source = target_archive_file(target, relative)
        if archive_name in archive_names:
            fail(f"duplicate release archive path: {archive_name}")
        archive_names.add(archive_name)

    # Archives carry the host tools the runner uses; build-only tools stay in the checkout.
    executables = {f"host/{name}" for name in platform["host"]["runtime_tools"].values()}
    required_runtime = {
        image,
        "runtime-manifest.json",
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
    }


def add_target_files(
    files: dict[str, bytes],
    target: str,
    relative_names: list[str],
) -> None:
    for relative in relative_names:
        archive_name, source = target_archive_file(target, relative)
        if archive_name in files:
            fail(f"duplicate release archive path: {archive_name}")
        files[archive_name] = source.read_bytes()


def _project_runtime(
    files: dict[str, bytes],
    *,
    host: str,
    ready_runtime: bool,
) -> None:
    """Admit only the runtime delivered for this host, preserving its kernel identity."""
    runtime = json.loads(files["runtime-manifest.json"])
    if host == "windows":
        runtime["host_tools"] = {
            role: WINDOWS_TOOLS.get(role, f"tools/{Path(relative).name}.exe")
            for role, relative in runtime["host_tools"].items()
            if role != "keyboard"
        }
    elif ready_runtime:
        runtime["host_tools"] = {
            role: path for role, path in runtime["host_tools"].items() if role != "keyboard"
        }
    native_files = {
        relative
        for relative in files
        if host == "linux" and relative.startswith(("runtime/", "tools/"))
    }
    runtime["sha256"] = {
        relative: sha256_bytes(files[relative])
        for relative in sorted(
            {*runtime["sha256"], *runtime["host_tools"].values(), *native_files}
        )
        if relative in files
    }
    files["runtime-manifest.json"] = canonical_json_bytes(runtime)
    if host == "windows":
        windows = json.loads(files["windows-host.json"])
        windows["sha256"] = {
            name: digest
            for name, digest in windows["sha256"].items()
            if name in files and name not in runtime["sha256"] and name != "README.txt"
        }
        files["windows-host.json"] = canonical_json_bytes(windows)


def _package_manifest(
    files: dict[str, bytes], manifest: dict[str, Any], executables: set[str]
) -> bytes:
    """Record the supplied archive members and preserve the build identity fields."""
    projected = {key: value for key, value in manifest.items() if key != "generation"}
    projected["files"] = {
        name: {
            "mode": 0o755 if name in executables else 0o644,
            "size": len(contents),
            "sha256": sha256_bytes(contents),
        }
        for name, contents in sorted(files.items())
    }
    projected["generation"] = sha256_bytes(canonical_json_bytes(projected))
    return canonical_json_bytes(projected)


def _write_archive(
    destination: Path,
    stem: str,
    files: dict[str, bytes],
    executables: set[str],
    *,
    compression: int = zipfile.ZIP_STORED,
) -> Path:
    """Publish a deterministic ZIP without replacing an existing different archive."""
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / f"{stem}.zip"
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination, prefix=f".{stem}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
        with zipfile.ZipFile(temporary, "w", compression=compression) as output:
            for relative in sorted(files):
                info = zipfile.ZipInfo(f"{stem}/{relative}", ZIP_TIMESTAMP)
                info.create_system = 3
                info.external_attr = (0o100755 if relative in executables else 0o100644) << 16
                info.compress_type = compression
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
    return archive


def package_target(
    target: str,
    *,
    profile: str | None = None,
    boot: str | None = None,
    host: str = "linux",
    output_directory: Path | None = None,
) -> None:
    if host not in {"linux", "windows"}:
        fail("package host must be linux or windows")
    build_type = "release"
    selected_profile = runtime_bundle_session.selected_context_profile(
        target, profile=profile, boot=boot
    )
    config = targets.load_target(target, selected_profile, build_type=build_type)
    ready_runtime = config["linux"]["root"]["kind"] == "initramfs"
    if ready_runtime and host not in runtime_hosts(config):
        fail(f"runtime archives do not support host {host} for this target")
    if not ready_runtime and host != "linux":
        fail("external-root exports support Linux x86-64 only")
    release_context = current_release_context()
    release = load_release_manifest(target, config)
    documentation = {} if ready_runtime else offline_documentation()
    bundle, manifest = runtime_bundle_session.resolve_target_bundle(
        target, selected_profile, build_type=build_type
    )
    if any(manifest.get(key) != value for key, value in release_context.fields().items()):
        fail("build release context is stale; rebuild this target before packaging")
    without_device_data = ready_runtime and config["platform"] == "sc9820e"
    if without_device_data:
        snapshot = workspaces.target_workspace_snapshot(
            target, selected_profile, build_type=build_type, include_device_data=False
        )
    else:
        snapshot = workspaces.target_workspace_snapshot(
            target, selected_profile, build_type=build_type
        )
    environment_recipe = definition.environment_recipe_digest()
    environment_state = environment_states.load_environment_state(
        common.ROOT / ".cache", environment_recipe
    )
    identity = cli_bundles.build_identity(snapshot, environment_state, common.ROOT / ".cache")
    if not cli_bundles.manifest_matches_identity(manifest, identity):
        clean_snapshot = workspaces.target_workspace_snapshot(
            target, selected_profile, build_type=build_type, include_device_data=False
        )
        identity = cli_bundles.build_identity(
            clean_snapshot, environment_state, common.ROOT / ".cache"
        )
    files_table = manifest.get("files")
    runtime_path = bundle.path / "runtime-manifest.json"
    if runtime_path.is_symlink() or not runtime_path.is_file():
        fail(f"release input is missing or invalid: {runtime_path}")
    try:
        runtime = json.loads(runtime_path.read_bytes())
    except (OSError, ValueError) as error:
        fail(f"release runtime manifest cannot be read: {error}")
    if not isinstance(runtime, dict) or not isinstance(runtime.get("sha256"), dict):
        fail("release runtime manifest has no executable closure")
    runtime_files = list(runtime["sha256"])
    try:
        required_boot_artifacts = cli_bundles.required_boot_artifacts(manifest)
    except ValueError as error:
        fail(str(error))
    reserved = {
        "build-manifest.json",
        "SHA256SUMS",
        *SHARED_PACKAGE_FILES,
    }
    collision = reserved & set(required_boot_artifacts)
    if collision:
        fail(f"profile boot artifact has a reserved archive path: {min(collision)}")
    bundle_files = list(
        dict.fromkeys((*release["bundle_files"], *required_boot_artifacts, *runtime_files))
    )
    if (
        not cli_bundles.manifest_matches_identity(manifest, identity)
        or not isinstance(files_table, dict)
        or not set(bundle_files).issubset(files_table)
    ):
        command = runtime_bundle_session.profile_command(
            "build", target, selected_profile, build_type=build_type
        )
        if without_device_data:
            command += " --without-device-data"
        fail(f"build output is stale; rebuild it: {command}")
    files: dict[str, bytes] = {}
    for relative in bundle_files:
        source = bundle.path / relative
        if source.is_symlink() or not source.is_file():
            fail(f"release input is missing or invalid: {source}")
        data = source.read_bytes()
        record = files_table[relative]
        if (
            not isinstance(record, dict)
            or record.get("sha256") != sha256_bytes(data)
            or record.get("size") != len(data)
            or record.get("mode") != (source.stat().st_mode & 0o777)
        ):
            fail(f"release input differs from its successful build manifest: {source}")
        files[relative] = data

    executables = {relative for relative in files if files_table[relative]["mode"] & 0o111}
    host_omissions = {
        relative
        for relative in files
        if (
            host == "linux"
            and (
                relative in {"Run.cmd", "scripts/fplinux_cli/runtime/windows_host.py"}
                or relative.startswith("common/windows/")
            )
        )
        or (host == "windows" and relative in {"Run.sh", "60-fplinux.rules"})
        or (not ready_runtime and relative == "Run.sh")
    }
    for relative in host_omissions:
        files.pop(relative)
    executables.difference_update(host_omissions)
    add_target_files(files, target, release["documents"])
    if not ready_runtime:
        files["README.txt"] = ordinary_readme(target, config)
        if config["platform"] == "sc9820e":
            from fplinux_cli.packaging.sources import (  # noqa: PLC0415
                collect_loader_license_notices,
            )

            files.update(collect_loader_license_notices(config))
    source_files: dict[str, bytes] = {}
    source_executables: set[str] = set()
    if ready_runtime:
        prepared_files = {"device-data.cpio"}
        for relative in prepared_files:
            files.pop(relative, None)
        omitted = {
            relative
            for relative in files
            if relative.startswith("apks/")
            or relative == "host/fplinux-usb-keyboard"
            or (host == "windows" and relative.startswith("host/"))
        }
        for relative in omitted:
            files.pop(relative)
        executables.difference_update(omitted)
        additions = autonomous_payload(config, host=host, runtime=runtime)
        collisions = (set(additions.files) & set(files)) - {"README.txt"}
        if collisions:
            fail(f"duplicate runtime archive path: {min(collisions)}")
        files.update(additions.files)
        executables.update(additions.executables)
        from fplinux_cli.packaging.sources import collect_source_files  # noqa: PLC0415

        sources = collect_source_files(config, host=host)
        source_files.update(sources.files)
        source_executables.update(sources.executables)
        files.update(sources.notices)
        for relative in tuple(files):
            if relative.startswith("sources/"):
                source_files[relative] = files.pop(relative)
    _project_runtime(files, host=host, ready_runtime=ready_runtime)
    collisions = set(files) & set(documentation)
    if collisions:
        fail(f"duplicate documentation archive path: {min(collisions)}")
    files.update(documentation)
    for archive_name, relative in SHARED_PACKAGE_FILES.items():
        source = common.ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"shared package file is missing or invalid: {source}")
        files[archive_name] = source.read_bytes()
    context_component = "" if selected_profile is None else f"-{selected_profile}"
    base_stem = f"FPLinux-{target}{context_component}-{host}-x64"
    source_stem = None
    if ready_runtime:
        source_stem = f"{base_stem}-{release_context.release_id}-sources"
        files["README.txt"] = append_source_companion(files["README.txt"], source_stem)
    finalize_runtime(files, manifest, executables)
    stem = f"{base_stem}-{release_context.release_id}"
    destination = (
        output_directory if output_directory is not None else common.ROOT / ".cache/out/releases"
    )
    archive = _write_archive(
        destination,
        stem,
        files,
        executables,
        compression=zipfile.ZIP_DEFLATED if ready_runtime else zipfile.ZIP_STORED,
    )
    if source_stem is not None:
        source_files["DISTRIBUTION.json"] = canonical_json_bytes(
            {
                **release_context.fields(),
                "runtimes": [distribution_record(archive, manifest, host)],
            }
        )
        finalize_checksums(source_files)
        source_archive = _write_archive(
            destination,
            source_stem,
            source_files,
            source_executables,
            compression=zipfile.ZIP_DEFLATED,
        )
        print(f"Matching sources: {common.display_text(source_archive)}")

    image_digest = sha256_bytes(files[release["image"]])
    print(f"FPLinux package: {common.display_text(archive)}")
    print(f"Archive SHA256: {sha256_file(archive)}")
    print(f"ramboot.bin SHA256: {image_digest}")

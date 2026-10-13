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
from fplinux_cli.cli import bundles as cli_bundles
from fplinux_cli.cli.autonomous_package import autonomous_payload
from fplinux_cli.cli.package_docs import offline_documentation
from fplinux_cli.cli.package_readme import append_source_companion, ordinary_readme
from fplinux_cli.common import (
    ZIP_TIMESTAMP,
    canonical_json_bytes,
    fail,
    payload_digest,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.environment import image_state as image_states
from fplinux_cli.environment import images
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.manifests.identity import RUNTIME_IDENTITY_PATH
from fplinux_cli.runtime import bundle_session as runtime_bundle_session
from fplinux_cli.workspace import build_inputs as workspaces

SHARED_PACKAGE_FILES = {
    "60-fplinux.rules": "common/60-fplinux.rules",
    "LICENSE": "LICENSE",
    "licenses/musl/COPYRIGHT": "third-party-licenses/musl/COPYRIGHT",
}


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
    executables = {
        "runner/run.py",
        *(f"host/{name}" for name in platform["host"]["runtime_tools"].values()),
    }
    required_runtime = {
        image,
        "runtime-manifest.json",
        "runner/platform_adapter.py",
        "runner/loader_events.py",
        *config["runtime"]["assets"].values(),
        *executables,
    }
    required_runtime.add(runtime_bundle_session.SSH_HELPER_PATH)
    required_runtime.add(RUNTIME_IDENTITY_PATH)
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


def _project_autonomous_runtime(
    files: dict[str, bytes], manifest: dict[str, Any], executables: set[str], *, host: str
) -> bytes:
    """Admit only the runtime delivered for this host, preserving its kernel identity."""
    runtime = json.loads(files["runtime-manifest.json"])
    runtime["host_tools"] = (
        {"loader": "tools/spd_dump.exe", "bridge": "tools/libc_server.exe"}
        if host == "windows10"
        else {role: runtime["host_tools"][role] for role in ("loader", "bridge")}
    )
    runtime["sha256"] = {
        relative: sha256_bytes(files[relative])
        for relative in sorted({*runtime["sha256"], *runtime["host_tools"].values()})
        if relative in files
    }
    files["runtime-manifest.json"] = canonical_json_bytes(runtime)
    if host == "windows10":
        windows = json.loads(files["windows-host.json"])
        windows["sha256"] = {
            name: digest
            for name, digest in windows["sha256"].items()
            if name in files and name not in runtime["sha256"] and name != "README.txt"
        }
        files["windows-host.json"] = canonical_json_bytes(windows)
    projected = {key: value for key, value in manifest.items() if key != "generation"}
    projected["files"] = {
        name: {
            "mode": 0o755 if name in executables else 0o644,
            "size": len(contents),
            "sha256": sha256_bytes(contents),
        }
        for name, contents in sorted(files.items())
        if name in runtime["sha256"] or name == "runtime-manifest.json"
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


def package_target(  # noqa: PLR0913 -- archive host and build selectors remain explicit.
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    boot: str | None = None,
    candidate: bool = False,
    autonomous: bool = False,
    host: str = "linux",
    offline: bool = False,
) -> None:
    if host not in {"linux", "windows10"}:
        fail("package host must be linux or windows10")
    if host == "windows10" and not autonomous:
        fail("--host windows10 requires --autonomous")
    if offline and not autonomous:
        fail("package --offline requires --autonomous")
    selected_profile = runtime_bundle_session.selected_context_profile(
        target, profile=profile, boot=boot
    )
    if selected_profile is not None and not candidate:
        fail("non-default boot contexts can only be packaged with --candidate")
    config = targets.load_target(target, selected_profile, build_type=build_type)
    if autonomous and config["linux"]["root"]["kind"] != "initramfs":
        fail("--autonomous supports only the RAM boot context")
    release = load_release_manifest(target, config)
    documentation = {} if autonomous else offline_documentation()
    bundle, manifest = runtime_bundle_session.resolve_target_bundle(
        target, selected_profile, build_type=build_type
    )
    snapshot = workspaces.target_workspace_snapshot(
        target, selected_profile, build_type=build_type
    )
    image_recipe = images.container_image_recipe_digest()
    image_state = image_states.load_image_state(common.ROOT / ".cache", image_recipe)
    identity = cli_bundles.build_identity(snapshot, image_state, common.ROOT / ".cache")
    files_table = manifest.get("files")
    prepared_map_files = tuple(
        relative
        for relative in ("pinmap.bin", "keymap.bin")
        if isinstance(files_table, dict) and relative in files_table
    )
    try:
        required_boot_artifacts = cli_bundles.required_boot_artifacts(manifest)
    except ValueError as error:
        fail(str(error))
    reserved = {
        "build-manifest.json",
        "CANDIDATE-NOTICE.txt",
        "SHA256SUMS",
        *SHARED_PACKAGE_FILES,
    }
    collision = reserved & set(required_boot_artifacts)
    if collision:
        fail(f"profile boot artifact has a reserved archive path: {min(collision)}")
    bundle_files = list(
        dict.fromkeys((*release["bundle_files"], *required_boot_artifacts, *prepared_map_files))
    )
    qualification_files = list(
        dict.fromkeys(
            (*release["qualification_files"], *required_boot_artifacts, *prepared_map_files)
        )
    )
    if (
        not cli_bundles.manifest_matches_identity(manifest, identity)
        or not isinstance(files_table, dict)
        or not set(bundle_files).issubset(files_table)
    ):
        command = runtime_bundle_session.profile_command(
            "build", target, selected_profile, build_type=build_type
        )
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

    executables = set(release["executables"])
    add_target_files(files, target, release["documents"])
    if not autonomous:
        files["README.txt"] = ordinary_readme(target, config)
    source_files: dict[str, bytes] = {}
    source_executables: set[str] = set()
    if autonomous:
        prepared_files = {"device-data.cpio", *prepared_map_files}
        for relative in prepared_files:
            files.pop(relative, None)
        omitted = {
            relative
            for relative in files
            if relative.startswith("apks/")
            or relative == "host/fplinux-usb-keyboard"
            or (host == "windows10" and relative.startswith("host/"))
        }
        for relative in omitted:
            files.pop(relative)
        executables.difference_update(omitted)
        qualification_files = [relative for relative in qualification_files if relative in files]
        additions = autonomous_payload(target, config, host=host, offline=offline)
        collisions = (set(additions.files) & set(files)) - {"README.txt"}
        if collisions:
            fail(f"duplicate autonomous archive path: {min(collisions)}")
        files.update(additions.files)
        qualification_files.extend(additions.files)
        executables.update(additions.executables)
        from fplinux_cli.cli.package_sources import collect_source_files  # noqa: PLC0415

        sources = collect_source_files(config, host=host, offline=offline)
        source_files.update(sources.files)
        source_executables.update(sources.executables)
        files.update(sources.notices)
        for relative in tuple(files):
            if relative.startswith("sources/"):
                source_files[relative] = files.pop(relative)
        qualification_files = [relative for relative in qualification_files if relative in files]
        package_manifest = _project_autonomous_runtime(files, manifest, executables, host=host)
    else:
        package_manifest = bundle.manifest_bytes
    qualification_payload = {relative: files[relative] for relative in qualification_files}
    qualification_digest = payload_digest(qualification_payload, executables)
    files["build-manifest.json"] = package_manifest
    verified_digest = None if candidate else releases.verified_runtime_digest(target)
    if not candidate and verified_digest != qualification_digest:
        fail(
            "this executable payload is not phone-tested for release; "
            "use --candidate for phone testing "
            f"(phone-test payload SHA256 {qualification_digest})"
        )

    if candidate:
        files["CANDIDATE-NOTICE.txt"] = (
            Path(__file__).with_suffix("") / "CANDIDATE-NOTICE.txt"
        ).read_bytes()
    collisions = set(files) & set(documentation)
    if collisions:
        fail(f"duplicate documentation archive path: {min(collisions)}")
    files.update(documentation)
    for archive_name, relative in SHARED_PACKAGE_FILES.items():
        if host == "windows10" and archive_name == "60-fplinux.rules":
            continue
        source = common.ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"shared package file is missing or invalid: {source}")
        files[archive_name] = source.read_bytes()
    qualifier = f"{build_type}-candidate" if candidate else build_type
    context = boot if boot is not None else selected_profile
    context_component = "" if context is None else f"-{context}"
    autonomous_component = "-autonomous" if autonomous else ""
    base_stem = (
        f"FPLinux-{target}{context_component}-{qualifier}{autonomous_component}-{host}-x86_64"
    )
    source_stem = None
    if autonomous:
        pairing_digest = payload_digest(
            {
                **{f"runtime/{name}": value for name, value in files.items()},
                **{f"source/{name}": value for name, value in source_files.items()},
            },
            {
                *(f"runtime/{name}" for name in executables),
                *(f"source/{name}" for name in source_executables),
            },
        )
        source_stem = f"{base_stem}-{pairing_digest[:16]}-sources"
        files["README.txt"] = append_source_companion(files["README.txt"], source_stem)
    checksums = "".join(f"{sha256_bytes(files[name])}  {name}\n" for name in sorted(files))
    files["SHA256SUMS"] = checksums.encode()
    content_digest = payload_digest(files, executables)
    stem = f"{base_stem}-{content_digest[:16]}"
    destination = common.ROOT / ".cache/out" / ("candidates" if candidate else "releases")
    archive = _write_archive(
        destination,
        stem,
        files,
        executables,
        compression=zipfile.ZIP_DEFLATED if autonomous else zipfile.ZIP_STORED,
    )
    if source_stem is not None:
        source_files["DISTRIBUTION.json"] = canonical_json_bytes(
            {"archive": archive.name, "archive_sha256": sha256_file(archive)}
        )
        source_checksums = "".join(
            f"{sha256_bytes(source_files[name])}  {name}\n" for name in sorted(source_files)
        )
        source_files["SHA256SUMS"] = source_checksums.encode()
        source_archive = _write_archive(
            destination,
            source_stem,
            source_files,
            source_executables,
            compression=zipfile.ZIP_DEFLATED,
        )
        print(f"Matching sources: {source_archive.relative_to(common.ROOT)}")

    image_digest = sha256_bytes(files[release["image"]])
    print(f"FPLinux {qualifier} package: {archive.relative_to(common.ROOT)}")
    print(f"Archive SHA256: {sha256_file(archive)}")
    print(f"Phone-test payload SHA256: {qualification_digest}")
    print(f"ramboot.bin SHA256: {image_digest}")

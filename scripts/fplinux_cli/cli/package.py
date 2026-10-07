# SPDX-License-Identifier: GPL-2.0-only
"""Package a selected build for standalone use."""

from __future__ import annotations

import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any

from fplinux_cli import common
from fplinux_cli.cli import bundles as cli_bundles
from fplinux_cli.common import ZIP_TIMESTAMP, fail, payload_digest, sha256_bytes, sha256_file
from fplinux_cli.environment import image_state as image_states
from fplinux_cli.environment import images
from fplinux_cli.manifests import platforms, releases, targets
from fplinux_cli.manifests.identity import RUNTIME_IDENTITY_PATH
from fplinux_cli.runtime import bundle_session as runtime_bundle_session
from fplinux_cli.workspace import build_inputs as workspaces

CANDIDATE_NOTICE = b"""PHONE-TEST CANDIDATE - DO NOT PUBLISH

This archive is for testing on the target phone only.
Candidate packaging does not make it release-ready.
"""


SHARED_PACKAGE_FILES = {
    "60-fplinux.rules": "common/60-fplinux.rules",
    "LICENSE": "LICENSE",
    "licenses/musl/COPYRIGHT": "THIRD_PARTY_LICENSES/musl/COPYRIGHT",
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
    manifest = releases.load_release(
        target, profile_packages=config.get("rootfs", {}).get("packages", ())
    )
    image = manifest["image"]
    bundle_files = manifest["bundle_files"]
    runtime_files = manifest["runtime_files"]
    documents = manifest["documents"]
    archive_names = set(bundle_files)
    shared_collisions = archive_names & SHARED_PACKAGE_FILES.keys()
    if shared_collisions:
        fail(f"duplicate release archive path: {min(shared_collisions)}")
    archive_names.update(SHARED_PACKAGE_FILES)
    for relative in documents:
        archive_name, _source = target_archive_file(target, relative)
        if archive_name in archive_names:
            fail(f"duplicate release archive path: {archive_name}")
        archive_names.add(archive_name)

    platform = platforms.load_platform(config["platform"])
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


def package_target(
    target: str,
    *,
    profile: str | None = None,
    build_type: str = "release",
    boot: str | None = None,
    candidate: bool = False,
) -> None:
    selected_profile = runtime_bundle_session.selected_context_profile(
        target, profile=profile, boot=boot
    )
    if selected_profile is not None and not candidate:
        fail("non-default boot contexts can only be packaged with --candidate")
    config = targets.load_target(target, selected_profile, build_type=build_type)
    release = load_release_manifest(target, config)
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
    bundle_files = list(dict.fromkeys((*release["bundle_files"], *required_boot_artifacts)))
    qualification_files = list(
        dict.fromkeys((*release["qualification_files"], *required_boot_artifacts))
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

    qualification_payload = {relative: files[relative] for relative in qualification_files}
    qualification_digest = payload_digest(qualification_payload, release["executables"])
    files["build-manifest.json"] = bundle.manifest_bytes
    verified_digest = None if candidate else releases.verified_runtime_digest(target)
    if not candidate and verified_digest != qualification_digest:
        fail(
            "this executable payload is not phone-tested for release; "
            "use --candidate for phone testing "
            f"(phone-test payload SHA256 {qualification_digest})"
        )

    add_target_files(files, target, release["documents"])
    if candidate:
        files["CANDIDATE-NOTICE.txt"] = CANDIDATE_NOTICE
    for archive_name, relative in SHARED_PACKAGE_FILES.items():
        source = common.ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"shared package file is missing or invalid: {source}")
        files[archive_name] = source.read_bytes()
    checksums = "".join(f"{sha256_bytes(files[name])}  {name}\n" for name in sorted(files))
    files["SHA256SUMS"] = checksums.encode()
    content_digest = payload_digest(files, release["executables"])

    qualifier = f"{build_type}-candidate" if candidate else build_type
    context = boot if boot is not None else selected_profile
    context_component = "" if context is None else f"-{context}"
    stem = f"FPLinux-{target}{context_component}-{qualifier}-linux-x86_64-{content_digest[:16]}"
    destination = common.ROOT / ".cache/out" / ("candidates" if candidate else "releases")
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
    print(f"FPLinux {qualifier} package: {archive.relative_to(common.ROOT)}")
    print(f"Archive SHA256: {sha256_file(archive)}")
    print(f"Phone-test payload SHA256: {qualification_digest}")
    print(f"ramboot.bin SHA256: {image_digest}")

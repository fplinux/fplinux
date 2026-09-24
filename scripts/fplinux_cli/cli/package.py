# SPDX-License-Identifier: GPL-2.0-only
"""Package a selected build for standalone use."""

from __future__ import annotations

import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any

from fplinux_cli import common
from fplinux_cli import image_state as image_states
from fplinux_cli import workspace as workspaces
from fplinux_cli.cli import bundles as bundles_commands
from fplinux_cli.cli import runtime as runtime_commands
from fplinux_cli.common import ZIP_TIMESTAMP, fail, payload_digest, sha256_bytes, sha256_file
from fplinux_cli.environment import images
from fplinux_cli.identity import RUNTIME_IDENTITY_PATH
from fplinux_cli.manifests import platforms, releases, targets

CANDIDATE_NOTICE = b"""PHONE-TEST CANDIDATE - DO NOT PUBLISH

This archive is for testing on the target phone only.
Candidate packaging does not make it release-ready.
"""


PACKAGE_DOCUMENTS = {
    "60-fplinux.rules": common.ROOT / "common/60-fplinux.rules",
    "LICENSE": common.ROOT / "LICENSE",
    "docs/apps/JPEG.md": common.ROOT / "docs/apps/JPEG.md",
    "docs/apps/PRESENT.md": common.ROOT / "docs/apps/PRESENT.md",
    "docs/apps/ROTATE.md": common.ROOT / "docs/apps/ROTATE.md",
    "docs/apps/MICROPYTHONOS.md": common.ROOT / "docs/apps/MICROPYTHONOS.md",
    "docs/apps/SHOWCASE.md": common.ROOT / "docs/apps/SHOWCASE.md",
    "docs/apps/TYRQUAKE.md": common.ROOT / "docs/apps/TYRQUAKE.md",
    "docs/features/AUXADC.md": common.ROOT / "docs/features/AUXADC.md",
    "docs/features/BATTERY_TELEMETRY.md": common.ROOT / "docs/features/BATTERY_TELEMETRY.md",
    "docs/features/BLUETOOTH.md": common.ROOT / "docs/features/BLUETOOTH.md",
    "docs/features/CHARGER_STATUS.md": common.ROOT / "docs/features/CHARGER_STATUS.md",
    "docs/features/CPU_CLOCK.md": common.ROOT / "docs/features/CPU_CLOCK.md",
    "docs/features/DISPLAY_BACKLIGHT.md": common.ROOT / "docs/features/DISPLAY_BACKLIGHT.md",
    "docs/features/FILE_TRANSFER.md": common.ROOT / "docs/features/FILE_TRANSFER.md",
    "docs/features/FM_RADIO.md": common.ROOT / "docs/features/FM_RADIO.md",
    "docs/features/SPEAKER_AUDIO.md": common.ROOT / "docs/features/SPEAKER_AUDIO.md",
    "docs/features/HEADPHONE_AUDIO.md": common.ROOT / "docs/features/HEADPHONE_AUDIO.md",
    "docs/features/HOST_KEYBOARD.md": common.ROOT / "docs/features/HOST_KEYBOARD.md",
    "docs/features/KEYPAD_BACKLIGHT.md": common.ROOT / "docs/features/KEYPAD_BACKLIGHT.md",
    "docs/features/LOCAL_CONSOLE.md": common.ROOT / "docs/features/LOCAL_CONSOLE.md",
    "docs/features/MICROPHONE_AUDIO.md": common.ROOT / "docs/features/MICROPHONE_AUDIO.md",
    "docs/features/MICROSD.md": common.ROOT / "docs/features/MICROSD.md",
    "docs/features/POWER_OFF.md": common.ROOT / "docs/features/POWER_OFF.md",
    "docs/features/RTC.md": common.ROOT / "docs/features/RTC.md",
    "docs/features/SOC_TEMPERATURE.md": common.ROOT / "docs/features/SOC_TEMPERATURE.md",
    "docs/features/SSH.md": common.ROOT / "docs/features/SSH.md",
    "docs/features/SUSPEND.md": common.ROOT / "docs/features/SUSPEND.md",
    "docs/features/USB_NETWORKING.md": common.ROOT / "docs/features/USB_NETWORKING.md",
    "docs/features/VIBRATION.md": common.ROOT / "docs/features/VIBRATION.md",
    "docs/guides/APK_PACKAGES.md": common.ROOT / "docs/guides/APK_PACKAGES.md",
    "docs/guides/STANDALONE.md": common.ROOT / "docs/guides/STANDALONE.md",
    "docs/guides/MICROSD_ROOT.md": common.ROOT / "docs/guides/MICROSD_ROOT.md",
    "licenses/musl/COPYRIGHT": common.ROOT / "THIRD_PARTY_LICENSES/musl/COPYRIGHT",
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
        if (
            len(source_name.parts) == 2
            and source_name.parts[0] == "features"
            and source_name.suffix == ".md"
        ):
            archive_name = f"docs/target/{source_name.name}"
        else:
            fail(
                "target package file must be below release/ or a direct "
                f"features/*.md file: {relative}"
            )
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
    manifest = releases.load_release(target)
    image = manifest["image"]
    bundle_files = manifest["bundle_files"]
    runtime_files = manifest["runtime_files"]
    documents = manifest["documents"]
    archive_names = set(bundle_files)
    shared_collisions = archive_names & PACKAGE_DOCUMENTS.keys()
    if shared_collisions:
        fail(f"duplicate release archive path: {min(shared_collisions)}")
    archive_names.update(PACKAGE_DOCUMENTS)
    for relative in documents:
        archive_name, _source = target_archive_file(target, relative)
        if archive_name in archive_names:
            fail(f"duplicate release archive path: {archive_name}")
        archive_names.add(archive_name)

    platform = platforms.load_platform(config["platform"])
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
    required_runtime.add(runtime_commands.SSH_HELPER_PATH)
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
        data = source.read_bytes()
        if relative.startswith("features/"):
            # Feature pages move from targets/<target>/features to docs/target.
            data = data.replace(b"](../../../docs/", b"](../")
        files[archive_name] = data


def package_target(
    target: str,
    *,
    profile: str | None = None,
    boot: str | None = None,
    candidate: bool = False,
) -> None:
    selected_profile = bundles_commands.selected_context_profile(
        target, profile=profile, boot=boot
    )
    if selected_profile is not None and not candidate:
        fail("non-default boot contexts can only be packaged with --candidate")
    config = targets.load_target(target, selected_profile)
    release = load_release_manifest(target, config)
    bundle, manifest = bundles_commands.resolve_target_bundle(target, selected_profile)
    snapshot = workspaces.target_workspace_snapshot(target, selected_profile)
    image_recipe = images.container_image_recipe_digest()
    image_state = image_states.load_image_state(common.ROOT / ".cache", image_recipe)
    identity = bundles_commands.build_identity(snapshot, image_state, common.ROOT / ".cache")
    files_table = manifest.get("files")
    try:
        required_boot_artifacts = bundles_commands.required_boot_artifacts(manifest)
    except ValueError as error:
        fail(str(error))
    reserved = {
        "build-manifest.json",
        "CANDIDATE-NOTICE.txt",
        "SHA256SUMS",
        *PACKAGE_DOCUMENTS,
    }
    collision = reserved & set(required_boot_artifacts)
    if collision:
        fail(f"profile boot artifact has a reserved archive path: {min(collision)}")
    bundle_files = list(dict.fromkeys((*release["bundle_files"], *required_boot_artifacts)))
    qualification_files = list(
        dict.fromkeys((*release["qualification_files"], *required_boot_artifacts))
    )
    if (
        not bundles_commands.manifest_matches_identity(manifest, identity)
        or not isinstance(files_table, dict)
        or not set(bundle_files).issubset(files_table)
    ):
        fail(
            "build output is stale; rebuild it: "
            f"{bundles_commands.profile_command('build', target, selected_profile)}"
        )
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
    for archive_name, source in PACKAGE_DOCUMENTS.items():
        if source.is_symlink() or not source.is_file():
            fail(f"release document is missing or invalid: {source}")
        data = source.read_bytes()
        if archive_name.endswith(".md"):
            # An archive owns its target status and loading instructions locally.
            data = data.replace(b"](../../targets/README.md)", b"](../../README.txt)")
            data = data.replace(b"](../guides/LOADING.md)", b"](../guides/STANDALONE.md)")
        files[archive_name] = data
    checksums = "".join(f"{sha256_bytes(files[name])}  {name}\n" for name in sorted(files))
    files["SHA256SUMS"] = checksums.encode()
    content_digest = payload_digest(files, release["executables"])

    qualifier = "candidate" if candidate else "release"
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

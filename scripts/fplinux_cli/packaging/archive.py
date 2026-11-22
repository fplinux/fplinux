# SPDX-License-Identifier: GPL-2.0-only
"""Project admitted builds and write deterministic ZIP archives."""

from __future__ import annotations

import json
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine.lock import bundle_runtime_packages, load_alpine_lock
from fplinux_cli.alpine.selection import bundle_packages, selected_base_packages
from fplinux_cli.common import (
    ZIP_TIMESTAMP,
    canonical_json_bytes,
    fail,
    sha256_bytes,
    sha256_file,
)
from fplinux_cli.manifests import platforms, releases
from fplinux_cli.packaging.boot_components import BOOT_COMPONENTS, boot_components
from fplinux_cli.runtime.windows_tools import WINDOWS_TOOLS

if TYPE_CHECKING:
    from fplinux_cli.packaging.autonomous import HostPayloads

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
    rootfs_packages = selected_base_packages(platform, config, root=common.ROOT)
    selected_bundle_packages = bundle_packages(platform, config, rootfs_packages, root=common.ROOT)
    bundle_apk_packages = (
        *selected_bundle_packages,
        *bundle_runtime_packages(
            load_alpine_lock(common.ROOT), rootfs_packages, selected_bundle_packages
        ),
    )
    manifest = releases.load_release(target, bundle_packages=bundle_apk_packages)
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


def read_bundle_members(
    bundle: Path, records: dict[str, Any], names: list[str]
) -> dict[str, bytes]:
    """Read the exact files admitted by a successful build manifest."""
    files = {}
    for relative in names:
        source = bundle / relative
        if source.is_symlink() or not source.is_file():
            fail(f"release input is missing or invalid: {source}")
        data = source.read_bytes()
        record = records.get(relative)
        if (
            not isinstance(record, dict)
            or record.get("sha256") != sha256_bytes(data)
            or record.get("size") != len(data)
            or record.get("mode") != (source.stat().st_mode & 0o777)
        ):
            fail(f"release input differs from its successful build manifest: {source}")
        files[relative] = data
    return files


def _project_runtime(
    files: dict[str, bytes],
    *,
    host: str,
) -> None:
    """Admit only the runtime delivered for this host, preserving its kernel identity."""
    runtime = json.loads(files["runtime-manifest.json"])
    if host == "windows":
        runtime["host_tools"] = {
            role: WINDOWS_TOOLS.get(role, f"tools/{Path(relative).name}.exe")
            for role, relative in runtime["host_tools"].items()
            if role != "keyboard"
        }
    else:
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


def project_bundle(  # noqa: PLR0913 -- the host and admitted build inputs stay explicit.
    target: str,
    config: dict[str, Any],
    bundle: Path,
    manifest: dict[str, Any],
    *,
    host: str,
    host_payloads: HostPayloads,
    card_files: dict[str, bytes] | None = None,
) -> tuple[dict[str, bytes], set[str], dict[str, bytes]]:
    """Project one admitted build into the shared host archive."""
    release = load_release_manifest(target, config)
    records = manifest["files"]
    runtime = json.loads(
        read_bundle_members(bundle, records, ["runtime-manifest.json"])["runtime-manifest.json"]
    )
    names = set(release["bundle_files"]) | runtime["sha256"].keys()
    names.discard("device-data.cpio")
    if host == "linux":
        names = {
            name
            for name in names
            if not name.startswith(("scripts/fplinux_cli/runtime/windows/", "common/windows/"))
            and name != "host/fplinux-usb-keyboard"
        }
    else:
        names = {
            name for name in names if not name.startswith("host/") and name != "60-fplinux.rules"
        }
    files = read_bundle_members(bundle, records, sorted(names))
    executables = {name for name in files if records[name]["mode"] & 0o111}
    add_target_files(files, target, release["documents"])

    if manifest["profile"] is None:
        files[BOOT_COMPONENTS] = boot_components(
            bundle,
            manifest,
            platform=config["platform"],
            image_path=release["image"],
            image=files[release["image"]],
        )
    additions = host_payloads.project(config, host=host, runtime=runtime)
    if collisions := additions.files.keys() & files.keys():
        fail(f"duplicate runtime archive path: {min(collisions)}")
    files.update(additions.files)
    executables.update(additions.executables)
    source_files = {}
    for name in tuple(files):
        if name.startswith("sources/"):
            source_files[name] = files.pop(name)
    _project_runtime(files, host=host)

    for name, path in SHARED_PACKAGE_FILES.items():
        source = common.ROOT / path
        if source.is_symlink() or not source.is_file():
            fail(f"shared package file is missing or invalid: {source}")
        files[name] = source.read_bytes()
    if card_files is not None:
        if collisions := card_files.keys() & files.keys():
            fail(f"duplicate system card archive path: {min(collisions)}")
        files.update(card_files)
    return files, executables, source_files

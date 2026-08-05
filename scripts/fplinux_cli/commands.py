# SPDX-License-Identifier: GPL-2.0-only
"""Build, package and run commands dispatched through target metadata."""

from __future__ import annotations

import json
import os
import tempfile
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any

from .common import (
    ROOT,
    ZIP_TIMESTAMP,
    fail,
    payload_digest,
    run,
    sha256_bytes,
    sha256_file,
)
from .config import (
    container_recipe_digest,
    load_container_lock,
    load_platform,
    load_release,
    load_target,
    verified_runtime_digest,
)
from .container import image_ready, require_podman, setup
from .workspace import stage_workspace

CANDIDATE_NOTICE = b"""HARDWARE QUALIFICATION CANDIDATE - DO NOT PUBLISH

This archive is for physical device qualification only.
Its runtime closure is not recorded as a release.
"""

PACKAGE_DOCUMENTS = {
    "LICENSE": ROOT / "LICENSE",
    "licenses/musl/COPYRIGHT": ROOT / "THIRD_PARTY_LICENSES/musl/COPYRIGHT",
}


def build(target: str, jobs: int) -> None:
    if jobs < 1:
        fail("--jobs must be positive")
    podman = require_podman()
    lock = load_container_lock()["oci"]
    if not image_ready(podman, lock["image"]):
        setup()
    cache = ROOT / ".cache"
    output = cache / "out"
    cache.mkdir(exist_ok=True)
    output.mkdir(exist_ok=True)
    workspace = stage_workspace(target)
    run(
        [
            podman,
            "run",
            "--rm",
            "--platform",
            lock["platform"],
            "--userns=keep-id",
            "--volume",
            f"{cache}:/cache:rw,Z",
            "--volume",
            f"{output}:/out:rw,Z",
            "--volume",
            f"{workspace}:/workspace:ro,Z",
            "--env",
            "HOME=/tmp/fplinux-home",
            "--env",
            "PYTHONPATH=/workspace/scripts",
            "--env",
            f"FPLINUX_CONTAINER_RECIPE={container_recipe_digest()}",
            lock["image"],
            "python3",
            "-m",
            "fplinux_cli.builder",
            "--target",
            target,
            "--jobs",
            str(jobs),
        ]
    )


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
    documents = manifest["documents"]
    archive_names = set(bundle_files)
    for relative in documents:
        archive_name, _source = target_archive_file(target, relative)
        if archive_name in archive_names:
            fail(f"duplicate release archive path: {archive_name}")
        archive_names.add(archive_name)
    if image not in bundle_files:
        fail("release manifest image must be a bundle file")

    platform = load_platform(config["platform"])
    executables = {
        "runner/run.py",
        *(f"host/{tool['name']}" for tool in platform["host"]["tools"]),
    }
    if not executables.issubset(bundle_files):
        fail("fixed platform executables must be release bundle files")
    return {
        "image": image,
        "bundle_files": bundle_files,
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
    bundle = ROOT / ".cache/out" / target / profile
    manifest_path = bundle / "build-manifest.json"
    if not manifest_path.is_file() or manifest_path.is_symlink():
        fail(f"build the target first: ./fplinux build {target}")
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, ValueError) as error:
        fail(f"build manifest is invalid: {error}")
    if not isinstance(manifest, dict):
        fail("build manifest root must be an object")
    workspace = stage_workspace(target)
    files_table = manifest.get("files")
    if (
        set(manifest)
        != {
            "format",
            "target",
            "profile",
            "workspace_recipe",
            "container_recipe",
            "files",
        }
        or manifest.get("format") != 1
        or manifest.get("target") != target
        or manifest.get("profile") != profile
        or manifest.get("workspace_recipe") != workspace.name
        or manifest.get("container_recipe") != container_recipe_digest()
        or not isinstance(files_table, dict)
        or set(files_table) != set(release["bundle_files"])
        or any(
            not isinstance(value, str)
            or len(value) != 64
            or any(character not in "0123456789abcdef" for character in value)
            for value in files_table.values()
        )
    ):
        fail(f"build output is stale; rebuild it: ./fplinux build {target}")

    files: dict[str, bytes] = {}
    for relative in release["bundle_files"]:
        source = bundle / relative
        if source.is_symlink() or not source.is_file():
            fail(f"release input is missing or invalid: {source}")
        data = source.read_bytes()
        if manifest["files"].get(relative) != sha256_bytes(data):
            fail(f"release input differs from its successful build manifest: {source}")
        files[relative] = data
    files["BUILD-MANIFEST.json"] = manifest_path.read_bytes()

    runtime_digest = payload_digest(files, release["executables"])
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
    runner = ROOT / ".cache/out" / target / config["profile"] / "runner/run.py"
    if runner.is_symlink() or not runner.is_file():
        fail(f"build the target first: ./fplinux build {target}")
    os.execv(os.fsencode(runner), [os.fsencode(runner)])

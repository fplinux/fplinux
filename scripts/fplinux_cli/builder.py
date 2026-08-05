#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: PLR0913, PLR0917
"""Build any declarative FPLinux target inside the pinned OCI environment."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import tarfile
import tempfile
import tomllib
import urllib.request
from pathlib import Path
from typing import Any, NoReturn

from .common import ROOT, sha256_file
from .config import exact_table, load_platform, load_release, load_target, relative_value

CACHE = Path("/cache")
OUTPUT = Path("/out")
SOURCE_DATE_EPOCH = "1784919600"
RUNTIME_MANIFEST_SCHEMA = "fplinux.runtime/v1"


def fail(message: str) -> NoReturn:
    """Stop a build without publishing a successful receipt."""
    raise SystemExit(f"build failed: {message}")


def require_file(path: Path) -> Path:
    """Require a regular, non-symlink file."""
    if path.is_symlink() or not path.is_file():
        fail(f"expected file is missing or invalid: {path}")
    return path


def require_directory(path: Path) -> Path:
    """Require a directory that is not a symlink."""
    if path.is_symlink() or not path.is_dir():
        fail(f"expected directory is missing or invalid: {path}")
    return path


def require_sha256(value: object, name: str) -> str:
    """Validate a lowercase SHA-256 digest."""
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return value


def root_source(relative: str) -> Path:
    """Resolve a repository-relative source file or directory."""
    return ROOT / relative_value(relative, "repository source path")


def target_source(target: str, relative: str) -> Path:
    """Resolve a target-relative source file or directory."""
    return ROOT / "targets" / target / relative_value(relative, "target source path")


def runner_source() -> Path:
    """Return the one shared runner source path."""
    return ROOT / "common/run.py"


def adapter_source(platform: str) -> Path:
    """Return the fixed adapter path for a validated platform."""
    return ROOT / "platforms" / platform / "host/adapter.py"


def build_environment() -> dict[str, str]:
    """Return the deterministic build environment."""
    environment = os.environ.copy()
    environment.update(
        {
            "LC_ALL": "C",
            "SOURCE_DATE_EPOCH": SOURCE_DATE_EPOCH,
            "KBUILD_BUILD_TIMESTAMP": "2026-07-24 19:00:00 +0000",
            "KBUILD_BUILD_USER": "fplinux",
            "KBUILD_BUILD_HOST": "builder",
            "KBUILD_BUILD_VERSION": "1",
            "KCONFIG_NOTIMESTAMP": "1",
        }
    )
    return environment


def run(command: list[str], *, cwd: Path | None = None) -> None:
    """Run one typed build step with deterministic environment variables."""
    print("+", " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(command, cwd=cwd, env=build_environment(), check=True)


def fetch(url: object, expected: object, cache: Path, name: object) -> Path:
    """Fetch one HTTPS resource into the validated shared download cache."""
    if not isinstance(url, str) or not url.startswith("https://"):
        fail("source URL must be a non-empty HTTPS URL")
    digest = require_sha256(expected, f"{name} source")
    relative = relative_value(name, "download cache name")
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if (
        destination.is_file()
        and not destination.is_symlink()
        and sha256_file(destination) == digest
    ):
        return destination
    if destination.exists() or destination.is_symlink():
        destination.unlink()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(  # noqa: S310 -- HTTPS is required above.
                url,
                headers={"User-Agent": "FPLinux/1"},
            )
            with urllib.request.urlopen(  # noqa: S310 -- HTTPS is required above.
                request,
                timeout=60,
            ) as response:
                shutil.copyfileobj(response, output)
        actual = sha256_file(temporary)
        if actual != digest:
            fail(f"{name} SHA256 mismatch: expected {digest}, received {actual}")
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def source_lock_entry(sources: dict[str, Any], name: object) -> dict[str, Any]:
    """Resolve a named source-lock entry without allowing a path or command."""
    if not isinstance(name, str) or not name:
        fail("source lock key must be a non-empty string")
    value = sources.get(name)
    if not isinstance(value, dict):
        fail(f"source lock entry is missing: {name}")
    return value


def integration_inputs(
    target: str, target_config: dict[str, Any], platform: dict[str, Any]
) -> list[tuple[str, str, str, Path]]:
    """Return typed Linux recipe inputs in projection order."""
    platform_linux = platform["linux"]
    target_linux = target_config["linux"]
    result = [
        ("platform-patch", relative, "", require_file(root_source(relative)))
        for relative in platform_linux["patches"]
    ]

    def add_steps(operation: str, steps: list[dict[str, Any]], *, platform_owned: bool) -> None:
        for step in steps:
            relative = step["source"]
            if platform_owned:
                identity = relative
                path = root_source(relative)
            else:
                identity = f"targets/{target}/{relative}"
                path = target_source(target, relative)
            result.append((operation, identity, step["destination"], require_file(path)))

    add_steps("platform-copy", platform_linux["copies"], platform_owned=True)
    add_steps("target-copy", target_linux["copies"], platform_owned=False)
    result.extend(
        (
            "target-patch",
            f"targets/{target}/{relative}",
            "",
            require_file(target_source(target, relative)),
        )
        for relative in target_linux["patches"]
    )
    add_steps("platform-append", platform_linux["appends"], platform_owned=True)
    add_steps("target-append", target_linux["appends"], platform_owned=False)
    return result


def linux_recipe_digest(
    linux_source: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> str:
    """Hash the pinned Linux release and every ordered projection operation."""
    version = linux_source.get("version")
    if not isinstance(version, str) or not version:
        fail("Linux source version must be a non-empty string")
    source_digest = require_sha256(linux_source.get("sha256"), "Linux source")
    manifest = {
        "version": version,
        "sha256": source_digest,
        "integration": [
            {
                "operation": operation,
                "source": relative,
                "destination": destination,
                "sha256": sha256_file(path),
            }
            for operation, relative, destination, path in integration_inputs(
                target, target_config, platform
            )
        ],
    }
    encoded = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def apply_patches(source: Path, paths: list[Path]) -> None:
    """Apply ordered, fuzz-free Linux patches."""
    for patch in paths:
        run(
            ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(patch)],
            cwd=source,
        )


def copy_steps(source: Path, steps: list[tuple[Path, str]]) -> None:
    """Project source files into a prepared Linux tree."""
    for source_path, destination_name in steps:
        destination = source / relative_value(destination_name, "Linux copy destination")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(require_file(source_path), destination)


def append_steps(source: Path, steps: list[tuple[Path, str]]) -> None:
    """Append Kconfig/Kbuild fragments in declared order."""
    for source_path, destination_name in steps:
        relative = relative_value(destination_name, "Linux append destination")
        destination = require_file(source / relative)
        with destination.open("ab") as output:
            output.write(b"\n")
            output.write(require_file(source_path).read_bytes())


def resolve_steps(
    target: str,
    steps: list[dict[str, Any]],
    *,
    platform_owned: bool,
) -> list[tuple[Path, str]]:
    """Resolve typed Linux projection steps from one ownership scope."""
    result: list[tuple[Path, str]] = []
    for step in steps:
        source = (
            root_source(step["source"])
            if platform_owned
            else target_source(target, step["source"])
        )
        result.append((require_file(source), step["destination"]))
    return result


def prepared_linux_recipe(source: Path) -> str | None:
    """Return the validated recipe of one target's prepared Linux tree."""
    if not source.exists() and not source.is_symlink():
        return None
    if source.is_symlink() or not source.is_dir():
        fail(f"prepared Linux path is invalid: {source}")
    marker = source / ".fplinux-recipe"
    if marker.is_symlink() or not marker.is_file():
        fail(f"prepared Linux tree is incomplete: {source}")
    return require_sha256(marker.read_text().strip(), "prepared Linux recipe")


def prepare_linux(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> tuple[Path, str]:
    """Create or replace the one recipe-validated Linux tree for a target."""
    platform_linux = platform["linux"]
    linux = source_lock_entry(sources, platform_linux["source_lock"])
    recipe = linux_recipe_digest(linux, target, target_config, platform)
    source = CACHE / "linux/sources" / target
    previous_recipe = prepared_linux_recipe(source)
    if previous_recipe == recipe:
        return source, recipe

    version = linux["version"]
    archive = fetch(
        linux.get("url"),
        linux.get("sha256"),
        CACHE / "downloads/linux",
        f"linux-{version}.tar.xz",
    )
    parent = source.parent
    parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(dir=parent, prefix=f".prepare-{target}-{recipe[:12]}-"))
    try:
        with tarfile.open(archive, "r:xz") as tar:
            tar.extractall(staging, filter="data")
        extracted = staging / f"linux-{version}"
        require_file(extracted / "Makefile")

        apply_patches(
            extracted,
            [require_file(root_source(relative)) for relative in platform_linux["patches"]],
        )
        copy_steps(
            extracted,
            [
                *resolve_steps(target, platform_linux["copies"], platform_owned=True),
                *resolve_steps(target, target_config["linux"]["copies"], platform_owned=False),
            ],
        )
        apply_patches(
            extracted,
            [
                require_file(target_source(target, relative))
                for relative in target_config["linux"]["patches"]
            ],
        )
        append_steps(
            extracted,
            [
                *resolve_steps(target, platform_linux["appends"], platform_owned=True),
                *resolve_steps(target, target_config["linux"]["appends"], platform_owned=False),
            ],
        )
        (extracted / ".fplinux-recipe").write_text(recipe + "\n")
        if previous_recipe is not None:
            shutil.rmtree(source)
        extracted.replace(source)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return source, recipe


def build_rootfs(
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    output: Path,
    jobs: int,
) -> tuple[Path, Path]:
    """Build the deterministic Buildroot rootfs and cross toolchain."""
    buildroot = platform["buildroot"]
    make_base = [
        "make",
        "-C",
        "/opt/buildroot",
        f"O={output}",
        f"BR2_EXTERNAL={root_source(buildroot['external'])}",
        f"BR2_DL_DIR={CACHE / 'downloads'}",
    ]
    defconfig = require_file(target_source(target, target_config["buildroot"]["defconfig"]))
    run([*make_base, f"BR2_DEFCONFIG={defconfig}", "defconfig"])
    for clean_target in buildroot["clean_targets"]:
        run([*make_base, clean_target])
    run([*make_base, f"-j{jobs}"])
    rootfs = require_file(output / "images/rootfs.cpio")
    cross = output / "host/bin" / platform["linux"]["cross_compile"]
    compiler = Path(str(cross) + "gcc")
    if not compiler.is_file():
        fail(f"cross compiler is missing: {compiler}")
    return rootfs, cross


def build_kernel(
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    linux_source: Path,
    output: Path,
    cross: Path,
    rootfs: Path,
    jobs: int,
) -> tuple[Path, Path]:
    """Build zImage and the declared target DTB."""
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(
        require_file(target_source(target, target_config["linux"]["defconfig"])),
        output / ".config",
    )
    kbuild = [
        "make",
        "-C",
        str(linux_source),
        f"O={output}",
        f"ARCH={platform['linux']['arch']}",
        f"CROSS_COMPILE={cross}",
    ]
    run([*kbuild, "olddefconfig"])
    run(
        [
            str(require_file(linux_source / platform["linux"]["config_script"])),
            "--file",
            str(output / ".config"),
            "--set-str",
            "INITRAMFS_SOURCE",
            str(rootfs),
        ]
    )
    run([*kbuild, "olddefconfig"])
    run([*kbuild, f"-j{jobs}", *platform["linux"]["targets"]])

    zimage = require_file(output / platform["linux"]["image_output"])
    dtb = require_file(
        output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
    )
    config_text = (output / ".config").read_text()
    for forbidden in target_config["linux"]["forbidden_config"]:
        if forbidden in config_text:
            fail(f"kernel unexpectedly contains {forbidden}")
    return zimage, dtb


def extract_vendor(archive: Path, prefix: str, files: list[str], output: Path) -> None:
    """Project the declared bootstrap vendor closure from a pinned archive."""
    with tarfile.open(archive, "r:gz") as source:
        for relative in files:
            member = source.getmember(prefix + relative)
            stream = source.extractfile(member)
            if stream is None or not member.isfile():
                fail(f"invalid bootstrap vendor member: {relative}")
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(stream.read())


def verify_images(
    ramboot: Path,
    zimage: Path,
    dtb: Path,
    map_file: Path,
    load_address: int,
    payload_limit: int,
) -> None:
    """Enforce the generic RAM-only bootstrap image contract."""
    image = ramboot.read_bytes()
    kernel = zimage.read_bytes()
    tree = dtb.read_bytes()
    if image[:4] != b"DHTB" or len(image) < 0x200:
        fail("bootstrap output does not have a complete DHTB header")
    declared = struct.unpack_from("<I", image, 0x30)[0]
    if declared + 0x200 > len(image):
        fail("DHTB declared size exceeds the RAM image")
    if load_address + len(image) >= payload_limit:
        fail("RAM image reaches its declared payload limit")
    if len(kernel) < 0x28 or kernel[0x24:0x28] != b"\x18\x28\x6f\x01":
        fail("zImage has invalid ARM magic")
    if tree[:4] != b"\xd0\x0d\xfe\xed":
        fail("target DTB has invalid FDT magic")
    lowered = tree.lower()
    for forbidden in (b"mmc", b"sdhci", b"sdio"):
        if forbidden in lowered:
            fail(f"target DTB contains forbidden storage marker {forbidden.decode()}")

    symbols: dict[str, int] = {}
    for line in map_file.read_text().splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            symbols[fields[2]] = int(fields[0], 16)
        except ValueError:
            continue
    required = {
        "__image_start",
        "linux_zimage_start",
        "linux_zimage_end",
        "linux_dtb_start",
        "linux_dtb_end",
        "FPLINUX_BOOTSTRAP_STORAGE_DISABLED",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        fail(f"bootstrap map lacks symbols: {', '.join(missing)}")
    if symbols["FPLINUX_BOOTSTRAP_STORAGE_DISABLED"] != 1:
        fail("bootstrap storage-disabled link marker is not one")
    if symbols["__image_start"] != load_address:
        fail("bootstrap map image start differs from target load_address")

    zimage_start = symbols["linux_zimage_start"]
    zimage_end = symbols["linux_zimage_end"]
    dtb_start = symbols["linux_dtb_start"]
    dtb_end = symbols["linux_dtb_end"]
    for name, start, end in (
        ("zImage", zimage_start, zimage_end),
        ("DTB", dtb_start, dtb_end),
    ):
        if not load_address <= start <= end <= load_address + len(image):
            fail(f"embedded {name} range lies outside the RAM image")
    if zimage_end - zimage_start != len(kernel):
        fail("embedded zImage size differs from the built zImage")
    if dtb_end - dtb_start != len(tree):
        fail("embedded DTB size differs from the built DTB")
    if image[zimage_start - load_address : zimage_end - load_address] != kernel:
        fail("embedded zImage bytes differ from the built zImage")
    if image[dtb_start - load_address : dtb_end - load_address] != tree:
        fail("embedded DTB bytes differ from the built DTB")


def build_bootstrap(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    work: Path,
    zimage: Path,
    dtb: Path,
) -> tuple[Path, Path]:
    """Build and verify the declarative bootstrap contract."""
    platform_bootstrap = platform["bootstrap"]
    target_bootstrap = target_config["bootstrap"]
    bootstrap_work = work / "bootstrap"
    if bootstrap_work.is_symlink():
        fail(f"generated bootstrap path must not be a symlink: {bootstrap_work}")
    if bootstrap_work.exists():
        if not bootstrap_work.is_dir():
            fail(f"generated bootstrap path is not a directory: {bootstrap_work}")
        shutil.rmtree(bootstrap_work)

    bootstrap = bootstrap_work / platform_bootstrap["source_destination"]
    vendor = bootstrap_work / platform_bootstrap["vendor_destination"]
    projected_output = bootstrap_work / platform_bootstrap["output_destination"]
    shutil.copytree(
        require_directory(target_source(target, target_bootstrap["source"])),
        bootstrap,
    )
    for step in platform_bootstrap["shared_copies"]:
        source = root_source(step["source"])
        destination = bootstrap / step["destination"]
        if source.is_dir() and not source.is_symlink():
            shutil.copytree(source, destination)
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(require_file(source), destination)

    vendor_lock = source_lock_entry(sources, platform_bootstrap["vendor_source_lock"])
    archive = fetch(
        vendor_lock.get("archive_url"),
        vendor_lock.get("archive_sha256"),
        CACHE / "downloads",
        platform_bootstrap["vendor_cache_name"],
    )
    commit = vendor_lock.get("commit")
    if not isinstance(commit, str) or not commit:
        fail("bootstrap vendor commit must be a non-empty string")
    prefix = platform_bootstrap["archive_prefix"].replace("{commit}", commit)
    extract_vendor(archive, prefix, platform_bootstrap["files"], vendor)

    projected_output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(zimage, projected_output / target_bootstrap["kernel_destination"])
    shutil.copyfile(dtb, projected_output / target_bootstrap["dtb_destination"])
    run(
        [
            "make",
            "-C",
            str(vendor / platform_bootstrap["pack_reloc"]),
            "clean",
            "all",
        ]
    )
    run(["make", "-C", str(bootstrap), platform_bootstrap["safety_target"]])
    run(
        [
            "make",
            "-C",
            str(bootstrap),
            *platform_bootstrap["build_targets"],
            f"TOOLCHAIN={target_bootstrap['toolchain']}",
            f"LTO={target_bootstrap['lto']}",
        ]
    )
    ramboot = require_file(bootstrap / target_bootstrap["image"])
    ramboot_map = require_file(bootstrap / target_bootstrap["map"])
    verify_images(
        ramboot,
        zimage,
        dtb,
        ramboot_map,
        target_bootstrap["load_address"],
        target_bootstrap["payload_limit"],
    )
    return ramboot, ramboot_map


def extract_7z_member(archive: Path, member: str) -> bytes:
    """Extract exactly one declared 7z member without a shell."""
    executable = shutil.which("7z") or shutil.which("7zz")
    if executable is None:
        fail("7z or 7zz is required")
    result = subprocess.run(
        [executable, "x", "-so", str(archive), member],
        capture_output=True,
        check=False,
    )
    if result.returncode:
        fail(result.stderr.decode(errors="replace").strip())
    return result.stdout


def write_checked(data: bytes, destination: Path, expected: str) -> None:
    """Atomically write bytes that match their declared digest."""
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        fail(f"{destination.name} SHA256 mismatch: expected {expected}, got {actual}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            output.write(data)
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def load_asset_lock(path: Path) -> list[dict[str, Any]]:
    """Load the generic pinned asset and extraction schema."""
    require_file(path)
    with path.open("rb") as stream:
        document = tomllib.load(stream)
    root = exact_table(document, {"schema", "source"}, "asset lock")
    if root.get("schema") != "fplinux.assets/v1":
        fail("asset lock schema must be fplinux.assets/v1")
    sources = root.get("source")
    if not isinstance(sources, list) or not sources:
        fail("asset lock source must be a non-empty array")

    source_ids: set[str] = set()
    roles: set[str] = set()
    paths: set[str] = set()
    for index, raw_source in enumerate(sources):
        name = f"asset source[{index}]"
        source = exact_table(
            raw_source,
            {"id", "kind", "url", "sha256", "cache_name", "license", "output"},
            name,
        )
        source_id = source.get("id")
        if not isinstance(source_id, str) or not source_id or source_id in source_ids:
            fail(f"{name} id must be a unique non-empty string")
        source_ids.add(source_id)
        kind = source.get("kind")
        if kind not in {"file", "7z"}:
            fail(f"{name} kind must be file or 7z")
        if not isinstance(source.get("url"), str) or not source["url"].startswith("https://"):
            fail(f"{name} url must use HTTPS")
        require_sha256(source.get("sha256"), f"{name} source")
        relative_value(source.get("cache_name"), f"{name} cache_name")
        if not isinstance(source.get("license"), str) or not source["license"]:
            fail(f"{name} license must be a non-empty string")
        outputs = source.get("output")
        if not isinstance(outputs, list) or not outputs:
            fail(f"{name} output must be a non-empty array")
        for output_index, raw_output in enumerate(outputs):
            output_name = f"{name} output[{output_index}]"
            keys = (
                {"role", "path", "sha256", "member"}
                if kind == "7z"
                else {
                    "role",
                    "path",
                    "sha256",
                }
            )
            output = exact_table(raw_output, keys, output_name)
            role = output.get("role")
            if not isinstance(role, str) or not role or role in roles:
                fail(f"{output_name} role must be a unique non-empty string")
            roles.add(role)
            relative = relative_value(output.get("path"), f"{output_name} path")
            if relative in paths:
                fail(f"asset output path is duplicated: {relative}")
            paths.add(relative)
            require_sha256(output.get("sha256"), f"{output_name} output")
            if kind == "7z":
                relative_value(output.get("member"), f"{output_name} member")
    return sources


def build_assets(lock_path: Path, output: Path) -> dict[str, tuple[str, str]]:
    """Fetch and extract every pinned asset through typed extractors."""
    result: dict[str, tuple[str, str]] = {}
    for source in load_asset_lock(lock_path):
        archive = fetch(
            source["url"],
            source["sha256"],
            CACHE / "downloads",
            source["cache_name"],
        )
        for item in source["output"]:
            expected = item["sha256"]
            data = (
                archive.read_bytes()
                if source["kind"] == "file"
                else extract_7z_member(archive, item["member"])
            )
            destination = output / item["path"]
            write_checked(data, destination, expected)
            result[item["role"]] = (item["path"], expected)
            print(f"{expected}  {destination}")
    return result


def extract_host_members(
    archive: Path,
    prefix: str,
    members: list[dict[str, Any]],
    hashes: dict[str, Any],
    output: Path,
) -> None:
    """Extract and verify a typed host-tool source projection."""
    output.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as source:
        for item in members:
            relative = item["path"]
            expected = require_sha256(hashes.get(item["digest_key"]), f"host member {relative}")
            member = source.getmember(prefix + relative)
            stream = source.extractfile(member)
            if stream is None or not member.isfile():
                fail(f"invalid host source member: {relative}")
            destination = output / Path(relative).name
            destination.write_bytes(stream.read())
            if sha256_file(destination) != expected:
                fail(f"host source hash mismatch: {relative}")


def build_make_host_tool(
    sources: dict[str, Any],
    recipe: dict[str, Any],
    work: Path,
    output: Path,
) -> Path:
    """Build one pinned make-archive host recipe."""
    source_lock = source_lock_entry(sources, recipe["source_lock"])
    archive = fetch(
        source_lock.get("archive_url"),
        source_lock.get("archive_sha256"),
        CACHE / "downloads",
        recipe["cache_name"],
    )
    commit = source_lock.get("commit")
    hashes = source_lock.get("files")
    if not isinstance(commit, str) or not commit:
        fail(f"host source {recipe['source_lock']} commit must be a non-empty string")
    if not isinstance(hashes, dict):
        fail(f"host source {recipe['source_lock']} files table is missing")
    prefix = recipe["archive_prefix"].replace("{commit}", commit)
    source = work / recipe["source_directory"]
    extract_host_members(archive, prefix, recipe["members"], hashes, source)
    run(["make", "-C", str(source), "clean", "all"])
    built = require_file(source / recipe["binary"])
    destination: Path = output / recipe["name"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built, destination)
    destination.chmod(0o755)
    return destination


def build_cc_libusb_tool(recipe: dict[str, Any], output: Path) -> Path:
    """Build one common C/libusb host capability recipe."""
    pkg = subprocess.run(
        ["pkg-config", "--cflags", "--libs", "libusb-1.0"],
        capture_output=True,
        text=True,
        env=build_environment(),
        check=False,
    )
    if pkg.returncode:
        fail(pkg.stderr.strip())
    destination: Path = output / recipe["name"]
    run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-O2",
            "-g0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fno-ident",
            "-o",
            str(destination),
            str(require_file(root_source(recipe["source"]))),
            *shlex.split(pkg.stdout),
            "-pthread",
        ]
    )
    destination.chmod(0o755)
    if recipe["self_test"]:
        run([str(destination), "--self-test"])
    return destination


def build_host_tools(
    sources: dict[str, Any], platform: dict[str, Any], work: Path
) -> dict[str, Path]:
    """Build every typed platform host capability recipe."""
    source_work = work / "host-build"
    output = work / "host"
    source_work.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    result: dict[str, Path] = {}
    for recipe in platform["host"]["tools"]:
        if recipe["type"] == "make-archive/v1":
            built = build_make_host_tool(sources, recipe, source_work, output)
        elif recipe["type"] == "cc-libusb/v1":
            built = build_cc_libusb_tool(recipe, output)
        else:
            fail(f"unsupported host recipe type: {recipe['type']}")
        result[recipe["name"]] = built
    return result


def copy_file(source: Path, destination: Path, *, executable: bool = False) -> None:
    """Copy a validated output with a normalized mode."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(require_file(source), destination)
    destination.chmod(0o755 if executable else 0o644)


def write_json(path: Path, value: dict[str, Any], *, prefix: str) -> None:
    """Atomically write deterministic JSON metadata."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=path.parent,
            prefix=prefix,
            mode="w",
            encoding="utf-8",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
        temporary.chmod(0o644)
        temporary.replace(path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def runtime_manifest(
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    image: str,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
) -> dict[str, Any]:
    """Create the generic runtime contract consumed by the common runner."""
    declared_assets = target_config["runtime"]["assets"]
    if set(declared_assets) != set(asset_outputs):
        fail("target runtime asset roles differ from the pinned asset lock")
    for role, bundle_path in declared_assets.items():
        expected = f"assets/{asset_outputs[role][0]}"
        if bundle_path != expected:
            fail(f"target runtime asset {role} must resolve to {expected}")

    platform_host = platform["host"]
    runtime_tools = {role: f"host/{name}" for role, name in platform_host["runtime_tools"].items()}
    for name in platform_host["runtime_tools"].values():
        if name not in host_tools:
            fail(f"runtime host tool was not built: {name}")
    hashes = {
        image: sha256_file(require_file(OUTPUT / target / target_config["profile"] / image)),
        "runner/platform_adapter.py": sha256_file(
            require_file(adapter_source(target_config["platform"]))
        ),
    }
    hashes.update(
        {
            declared_assets[role]: digest_value
            for role, (_relative, digest_value) in asset_outputs.items()
        }
    )
    hashes.update(
        {
            runtime_tools[role]: sha256_file(host_tools[name])
            for role, name in platform_host["runtime_tools"].items()
        }
    )
    runtime = target_config["runtime"]
    return {
        "schema": RUNTIME_MANIFEST_SCHEMA,
        "target": target,
        "display_name": target_config["display_name"],
        "platform": target_config["platform"],
        "capability": platform_host["capability"],
        "image": image,
        "addresses": {
            "fdl1": runtime["fdl1_load_address"],
            "payload": target_config["bootstrap"]["load_address"],
        },
        "usb": runtime["usb"],
        "assets": declared_assets,
        "adapter": runtime["adapter"],
        "host_tools": runtime_tools,
        "sha256": hashes,
    }


def publish_bundle(
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    release_manifest: dict[str, Any],
    work: Path,
    rootfs: Path,
    kernel_output: Path,
    zimage: Path,
    dtb: Path,
    ramboot: Path,
    ramboot_map: Path,
    asset_lock_path: Path,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
) -> Path:
    """Publish built outputs and write the successful receipt last."""
    release: Path = OUTPUT / target / target_config["profile"]
    if release.is_symlink():
        fail(f"generated release path must not be a symlink: {release}")
    if release.exists():
        if not release.is_dir():
            fail(f"generated release path is not a directory: {release}")
        shutil.rmtree(release)
    release.mkdir(parents=True)

    image_name = release_manifest["image"]
    copy_file(ramboot, release / image_name)
    for source, name in (
        (zimage, "zImage"),
        (dtb, target_config["linux"]["debug_dtb"]),
        (rootfs, "rootfs.cpio"),
        (kernel_output / "vmlinux", "vmlinux"),
        (kernel_output / "System.map", "System.map"),
        (kernel_output / ".config", "kernel.config"),
        (ramboot_map, "ramboot.map"),
    ):
        copy_file(source, release / "debug" / name)
    for relative, _digest in asset_outputs.values():
        copy_file(work / "assets" / relative, release / "assets" / relative)
    for name, source in host_tools.items():
        copy_file(source, release / "host" / name, executable=True)
    copy_file(runner_source(), release / "runner/run.py", executable=True)
    copy_file(
        adapter_source(target_config["platform"]),
        release / "runner/platform_adapter.py",
    )
    copy_file(asset_lock_path, release / "assets.lock.toml")
    copy_file(ROOT / "THIRD_PARTY_NOTICES.md", release / "THIRD_PARTY_NOTICES.md")

    runtime = runtime_manifest(
        target,
        target_config,
        platform,
        image_name,
        asset_outputs,
        host_tools,
    )
    write_json(release / "runtime-manifest.json", runtime, prefix=".runtime-manifest.")

    for relative in release_manifest["bundle_files"]:
        require_file(release / relative)

    workspace_marker = ROOT / ".fplinux-workspace"
    if not workspace_marker.is_file() or workspace_marker.is_symlink():
        fail("staged workspace recipe marker is missing")
    workspace_recipe = workspace_marker.read_text().strip()
    container_recipe = os.environ.get("FPLINUX_CONTAINER_RECIPE", "")
    require_sha256(workspace_recipe, "workspace recipe")
    require_sha256(container_recipe, "container recipe")
    manifest = {
        "format": 1,
        "target": target,
        "profile": target_config["profile"],
        "workspace_recipe": workspace_recipe,
        "container_recipe": container_recipe,
        "files": {
            relative: sha256_file(require_file(release / relative))
            for relative in release_manifest["bundle_files"]
        },
    }
    write_json(release / "build-manifest.json", manifest, prefix=".build-manifest.")
    return release


def main() -> None:
    """Build stages 1-4 and publish one deterministic target bundle."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--jobs", type=int, required=True)
    args = parser.parse_args()
    if args.jobs < 1:
        fail("jobs must be positive")

    target_config = load_target(args.target)
    platform = load_platform(target_config["platform"])
    with (ROOT / "sources.lock.toml").open("rb") as stream:
        sources = tomllib.load(stream)
    target_directory = ROOT / "targets" / args.target
    asset_lock_path = require_file(target_directory / target_config["assets_lock"])
    release_manifest = load_release(args.target, target_config)

    work = OUTPUT / args.target / "work"
    work.mkdir(parents=True, exist_ok=True)
    CACHE.mkdir(parents=True, exist_ok=True)
    linux_source, linux_recipe = prepare_linux(
        sources,
        args.target,
        target_config,
        platform,
    )

    rootfs, cross = build_rootfs(
        args.target,
        target_config,
        platform,
        work / "buildroot",
        args.jobs,
    )
    kernel_output = work / f"kernel-{linux_recipe[:16]}"
    zimage, dtb = build_kernel(
        args.target,
        target_config,
        platform,
        linux_source,
        kernel_output,
        cross,
        rootfs,
        args.jobs,
    )
    ramboot, ramboot_map = build_bootstrap(
        sources,
        args.target,
        target_config,
        platform,
        work,
        zimage,
        dtb,
    )
    asset_outputs = build_assets(asset_lock_path, work / "assets")
    host_tools = build_host_tools(sources, platform, work)
    release = publish_bundle(
        args.target,
        target_config,
        platform,
        release_manifest,
        work,
        rootfs,
        kernel_output,
        zimage,
        dtb,
        ramboot,
        ramboot_map,
        asset_lock_path,
        asset_outputs,
        host_tools,
    )
    print(f"FPLinux build complete: {release}")
    print(f"ramboot.bin SHA256: {sha256_file(release / release_manifest['image'])}")


if __name__ == "__main__":
    main()

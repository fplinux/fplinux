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
from contextlib import contextmanager
from pathlib import Path
from typing import TYPE_CHECKING, Any, NoReturn

from . import alpine_builder, alpine_state, kbuild_state, linux_state
from .bundle_state import (
    canonical_json_bytes,
    create_bundle_staging,
    discard_bundle_staging,
    publish_bundle_generation,
    publish_current_bundle,
    published_file_records,
)
from .common import ROOT, sha256_bytes, sha256_file
from .config import (
    load_asset_lock,
    load_platform,
    load_release,
    load_target,
    relative_value,
    target_asset_lock_path,
    target_defconfig_path,
)
from .device_state import DeviceStateError, device_kernel_identity, localversion
from .kbuild_state import KbuildStateError
from .linux_state import LinuxStateError, PreparedLinuxState
from .output import RunReporter, current_stage, run_entrypoint

if TYPE_CHECKING:
    from collections.abc import Iterator

CACHE = Path("/cache")
OUTPUT = Path("/out")
SOURCE_DATE_EPOCH = "1784919600"
RAM_SESSION_BYTES = 512
RAM_SESSION_ALIGNMENT = 64

RAM_SESSION_RNG_SEED_MARKER = bytes([0xA1]) * 64
RAM_SESSION_DTB_MARKERS = {
    "fplinux,ssh-client-key": bytes([0xB2]) * 68,
    "fplinux,session-id": bytes([0xC3]) * 32,
    "fplinux,usb-session": bytes([0xD4]) * 256,
}


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


def ssh_transport_source() -> Path:
    """Return the SSH session helper published beside the runner."""
    return ROOT / "scripts/fplinux_cli/ssh_transport.py"


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


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    environment: dict[str, str] | None = None,
) -> None:
    """Run one typed build step with deterministic environment variables."""
    effective_environment = build_environment()
    if environment is not None:
        effective_environment.update(environment)
    stage = current_stage()
    if stage is not None:
        stage.run(command, cwd=cwd, env=effective_environment)
        return
    print("+", " ".join(shlex.quote(part) for part in command), flush=True)
    subprocess.run(command, cwd=cwd, env=effective_environment, check=True)


@contextmanager
def report_stage(reporter: RunReporter | None, name: str) -> Iterator[None]:
    """Group typed build commands into one persistent stage log."""
    if reporter is None:
        yield
        return
    with reporter.stage(name):
        yield


def log_message(message: str) -> None:
    """Keep routine build details in the active stage log."""
    stage = current_stage()
    if stage is None:
        print(message)
        return
    stage.write((message + "\n").encode())


def fetch(url: object, expected: object, cache: Path, name: object) -> Path:
    """Fetch one HTTPS resource into the validated shared download cache."""
    if not isinstance(url, str) or not url.startswith("https://"):
        fail("source URL must be a non-empty HTTPS URL")
    digest = require_sha256(expected, f"{name} source")
    relative = relative_value(name, "download cache name")
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_file():
            fail(f"download cache destination is invalid: {destination}")
        if sha256_file(destination) == digest:
            return destination
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
                headers={"User-Agent": "FPLinux"},
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


def bootstrap_tree_entries(source: Path) -> list[dict[str, int | str]]:
    """Describe the bytes and modes copied from one bootstrap source tree."""
    source = require_directory(source)
    entries: list[dict[str, int | str]] = [
        {"path": ".", "type": "directory", "mode": source.stat().st_mode & 0o777}
    ]
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source).as_posix()
        if path.is_dir():
            entries.append(
                {
                    "path": relative,
                    "type": "directory",
                    "mode": path.stat().st_mode & 0o777,
                }
            )
        elif path.is_file():
            entries.append(
                {
                    "path": relative,
                    "type": "file",
                    "mode": path.stat().st_mode & 0o777,
                    "sha256": sha256_file(path),
                }
            )
        else:
            fail(f"bootstrap source entry is not a regular file or directory: {path}")
    return entries


def bootstrap_recipe_digest(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> str:
    """Hash exactly the bootstrap inputs which can change the RAM image."""
    platform_bootstrap = platform["bootstrap"]
    target_bootstrap = target_config["bootstrap"]
    vendor_source = source_lock_entry(sources, platform_bootstrap["vendor_source_lock"])
    vendor_commit = vendor_source.get("commit")
    if not isinstance(vendor_commit, str) or not vendor_commit:
        fail("bootstrap vendor commit must be a non-empty string")
    shared_copies: list[dict[str, object]] = []
    for step in platform_bootstrap["shared_copies"]:
        source = root_source(step["source"])
        copied: dict[str, object] = {
            "source": step["source"],
            "destination": step["destination"],
        }
        if source.is_dir() and not source.is_symlink():
            copied["tree"] = bootstrap_tree_entries(source)
        else:
            copied["sha256"] = sha256_file(require_file(source))
        shared_copies.append(copied)
    manifest = {
        "target": target,
        "target_bootstrap": target_bootstrap,
        "platform_bootstrap": platform_bootstrap,
        "target_source": bootstrap_tree_entries(target_source(target, "bootstrap")),
        "shared_copies": shared_copies,
        "vendor_source": {
            "commit": vendor_commit,
            "archive_sha256": require_sha256(
                vendor_source.get("archive_sha256"),
                "bootstrap vendor source",
            ),
        },
        # build_bootstrap() is in this file, already part of the Kbuild plan.
        "implementation": {"scripts/fplinux_cli/builder.py": sha256_file(Path(__file__))},
    }
    return sha256_bytes(canonical_json_bytes(manifest))


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


def prepare_linux(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
) -> tuple[Path, PreparedLinuxState]:
    """Create or exactly reuse the one receipt-validated Linux tree for a target."""
    platform_linux = platform["linux"]
    linux = source_lock_entry(sources, platform_linux["source_lock"])
    recipe = linux_recipe_digest(linux, target, target_config, platform)
    version = linux["version"]
    source_digest = require_sha256(linux.get("sha256"), "Linux source")
    platform_patches = [
        require_file(root_source(relative)) for relative in platform_linux["patches"]
    ]
    target_patches = [
        require_file(target_source(target, relative))
        for relative in target_config["linux"]["patches"]
    ]
    copies = [
        *resolve_steps(target, platform_linux["copies"], platform_owned=True),
        *resolve_steps(target, target_config["linux"]["copies"], platform_owned=False),
    ]
    appends = [
        *resolve_steps(target, platform_linux["appends"], platform_owned=True),
        *resolve_steps(target, target_config["linux"]["appends"], platform_owned=False),
    ]
    try:
        parent = linux_state.ensure_sources_directory(CACHE)
    except LinuxStateError as error:
        fail(str(error))
    source = parent / target

    def apply_projection(destination: Path) -> None:
        apply_patches(destination, platform_patches)
        copy_steps(destination, copies)
        apply_patches(destination, target_patches)
        append_steps(destination, appends)

    prepared = linux_state.inspect_prepared_linux(source, recipe)
    if prepared is not None:
        return source, prepared

    archive = fetch(
        linux.get("url"),
        source_digest,
        CACHE / "downloads/linux",
        f"linux-{version}.tar.xz",
    )
    staging = Path(tempfile.mkdtemp(dir=parent, prefix=f".prepare-{target}-{recipe[:12]}-"))
    try:
        with tarfile.open(archive, "r:xz") as tar:
            tar.extractall(staging, filter="data")
        extracted = staging / f"linux-{version}"
        require_file(extracted / "Makefile")
        try:
            apply_projection(extracted)
            state = linux_state.seal_prepared_linux(extracted, recipe)
            linux_state.publish_prepared_linux(source, extracted)
        except LinuxStateError as error:
            fail(str(error))
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return source, state


def kernel_build_commands(
    kbuild: list[str],
    config_command: list[str],
    targets: list[str],
    jobs: int,
) -> list[list[str]]:
    """Return the exact ordered Kbuild argv used by both the plan and executor."""
    if jobs < 1:
        fail("Kbuild jobs must be positive")
    return [
        [*kbuild, "olddefconfig"],
        config_command,
        [*kbuild, "olddefconfig"],
        [*kbuild, f"-j{jobs}", *targets],
    ]


def build_kernel(
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    bootstrap_recipe: str,
    linux_source: Path,
    prepared_linux: PreparedLinuxState,
    linux_base: str,
    output: Path,
    cross: str,
    rootfs: Path,
    rootfs_output: Path,
    rootfs_recipe: str,
    jobs: int,
) -> tuple[Path, Path, dict[str, str], str]:
    """Build or exactly reuse zImage and the declared target DTB in ``work/kernel``."""
    try:
        work = output.parent
        defconfig = require_file(target_defconfig_path(target))
        rootfs_record = kbuild_state.rootfs_identity(rootfs)
        rootfs_input = kbuild_state.rootfs_input_path(work, rootfs_record)
        rootfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
        kbuild = [
            "make",
            "-C",
            str(linux_source),
            f"O={output}",
            f"ARCH={platform['linux']['arch']}",
            f"CROSS_COMPILE={cross}",
        ]
        config_script = require_file(linux_source / platform["linux"]["config_script"])
        current_linux = linux_state.require_prepared_linux(linux_source, prepared_linux)
        implementation = [
            ("scripts/fplinux_cli/builder.py", Path(__file__)),
            (
                "scripts/fplinux_cli/device_state.py",
                Path(__file__).with_name("device_state.py"),
            ),
            ("scripts/fplinux_cli/kbuild_state.py", Path(kbuild_state.__file__)),
        ]
        device_identity = device_kernel_identity(
            target=target,
            linux_recipe=current_linux.linux_recipe,
            bootstrap_recipe=bootstrap_recipe,
            rootfs=rootfs_record,
            rootfs_receipt=rootfs_receipt,
            kbuild_implementation=kbuild_state.implementation_identity(implementation),
            arch=platform["linux"]["arch"],
            defconfig=defconfig,
            dtb=target_config["linux"]["dtb"],
        )
        config_command = [
            str(config_script),
            "--file",
            str(output / ".config"),
            "--set-str",
            "INITRAMFS_SOURCE",
            str(rootfs_input),
            "--set-str",
            "LOCALVERSION",
            localversion(device_identity),
        ]
        commands = kernel_build_commands(
            kbuild,
            config_command,
            platform["linux"]["targets"],
            jobs,
        )
        output_paths = (
            platform["linux"]["image_output"],
            str(Path(platform["linux"]["dtb_output_directory"]) / target_config["linux"]["dtb"]),
            "vmlinux",
            "System.map",
            ".config",
        )
        plan = kbuild_state.create_plan(
            linux_recipe=current_linux.linux_recipe,
            linux_base=require_sha256(linux_base, "Linux base source"),
            defconfig=defconfig,
            defconfig_path=f"targets/{target}/kernel/defconfig",
            rootfs=rootfs_record,
            rootfs_input=rootfs_input,
            rootfs_receipt=rootfs_receipt,
            arch=platform["linux"]["arch"],
            cross_compile=cross,
            commands=commands,
            outputs=output_paths,
            implementation=implementation,
        )
        if kbuild_state.cache_hit(work, output, plan):
            log_message(f"Kbuild causal receipt hit: {plan.recipe[:16]}")
        else:
            kbuild_state.discard_success_receipt(work)
            kbuild_state.prepare_output(work, output)
            kbuild_state.materialize_rootfs_input(work, rootfs, plan)
            shutil.copyfile(defconfig, output / ".config")
            for command in commands:
                run(command)

        zimage = require_file(output / platform["linux"]["image_output"])
        dtb = require_file(
            output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
        )
        config_text = require_file(output / ".config").read_text()
        for forbidden in target_config["linux"]["forbidden_config"]:
            if forbidden in config_text:
                fail(f"kernel unexpectedly contains {forbidden}")
        if not kbuild_state.cache_hit(work, output, plan):
            kbuild_state.publish_success(work, output, plan)
        return zimage, dtb, kbuild_state.receipt_identity(work, output, plan), device_identity
    except (DeviceStateError, KbuildStateError, LinuxStateError) as error:
        fail(str(error))


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


def _fdt_session_properties(tree: bytes) -> tuple[dict[str, bytes], dict[str, bytes]]:
    """Read exact properties from /chosen and /fplinux-session."""
    if len(tree) < 40:
        fail("target DTB does not have a complete FDT header")
    (
        magic,
        total_size,
        structure_offset,
        strings_offset,
        _reserved_offset,
        version,
        _last_compatible_version,
        _boot_cpu,
        strings_size,
        structure_size,
    ) = struct.unpack_from(">10I", tree)
    if magic != 0xD00DFEED or total_size != len(tree) or version < 17:
        fail("target DTB has an invalid FDT header")
    structure_end = structure_offset + structure_size
    strings_end = strings_offset + strings_size
    if (
        structure_offset < 40
        or structure_end > len(tree)
        or strings_offset < 40
        or strings_end > len(tree)
    ):
        fail("target DTB has invalid block bounds")

    position = structure_offset
    stack: list[str] = []
    paths: dict[tuple[str, ...], dict[str, bytes]] = {
        ("", "chosen"): {},
        ("", "fplinux-session"): {},
    }
    seen: set[tuple[str, ...]] = set()
    while position + 4 <= structure_end:
        token = struct.unpack_from(">I", tree, position)[0]
        position += 4
        if token == 1:  # FDT_BEGIN_NODE
            terminator = tree.find(b"\0", position, structure_end)
            if terminator < 0:
                fail("target DTB has an unterminated node name")
            try:
                name = tree[position:terminator].decode("ascii")
            except UnicodeDecodeError:
                fail("target DTB has a non-ASCII node name")
            stack.append(name)
            path = tuple(stack)
            if path in paths:
                if path in seen:
                    fail(f"target DTB repeats node /{'/'.join(path[1:])}")
                seen.add(path)
            position = (terminator + 4) & ~3
        elif token == 2:  # FDT_END_NODE
            if not stack:
                fail("target DTB has an unmatched end-node token")
            stack.pop()
        elif token == 3:  # FDT_PROP
            if position + 8 > structure_end:
                fail("target DTB has a truncated property header")
            value_size, name_offset = struct.unpack_from(">II", tree, position)
            position += 8
            value_end = position + value_size
            if value_end > structure_end or name_offset >= strings_size:
                fail("target DTB has an invalid property")
            name_start = strings_offset + name_offset
            name_end = tree.find(b"\0", name_start, strings_end)
            if name_end < 0:
                fail("target DTB has an unterminated property name")
            try:
                name = tree[name_start:name_end].decode("ascii")
            except UnicodeDecodeError:
                fail("target DTB has a non-ASCII property name")
            path = tuple(stack)
            if path in paths:
                properties = paths[path]
                if name in properties:
                    fail(f"target DTB /{'/'.join(path[1:])} repeats property {name}")
                properties[name] = tree[position:value_end]
            position = (value_end + 3) & ~3
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            if stack:
                fail("target DTB ends before all nodes are closed")
            missing = paths.keys() - seen
            if missing:
                path = min(missing)
                fail(f"target DTB lacks node /{'/'.join(path[1:])}")
            return paths[("", "chosen")], paths[("", "fplinux-session")]
        else:
            fail(f"target DTB has unknown structure token {token}")
    fail("target DTB lacks its final structure token")


def _verify_session_dtb(tree: bytes) -> None:
    """Require one canonical marker for each RAM-session DT property."""
    chosen, session = _fdt_session_properties(tree)
    if chosen.get("rng-seed") != RAM_SESSION_RNG_SEED_MARKER:
        fail("target DTB /chosen rng-seed does not contain its canonical marker")
    if tree.count(RAM_SESSION_RNG_SEED_MARKER) != 1:
        fail("target DTB marker rng-seed must occur exactly once")
    if session.get("compatible") != b"fplinux,ram-session\0":
        fail("target DTB /fplinux-session has an invalid compatible")
    for name, marker in RAM_SESSION_DTB_MARKERS.items():
        if session.get(name) != marker:
            fail(f"target DTB /fplinux-session {name} does not contain its canonical marker")
        if tree.count(marker) != 1:
            fail(f"target DTB marker {name} must occur exactly once")


def verify_images(
    ramboot: Path,
    zimage: Path,
    dtb: Path,
    map_file: Path,
    load_address: int,
    payload_limit: int,
    forbidden_markers: list[str],
) -> dict[str, int | str]:
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
    for marker in forbidden_markers:
        if marker.lower().encode() in lowered:
            fail(f"target DTB contains forbidden storage marker {marker}")

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
        "fplinux_session_start",
        "fplinux_session_end",
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

    session_start = symbols["fplinux_session_start"]
    session_end = symbols["fplinux_session_end"]
    expected_start = (dtb_end + RAM_SESSION_ALIGNMENT - 1) & -RAM_SESSION_ALIGNMENT
    if session_start != expected_start or session_start % RAM_SESSION_ALIGNMENT:
        fail("RAM-session slot does not immediately follow the aligned embedded DTB")
    if session_end - session_start != RAM_SESSION_BYTES:
        fail("RAM-session slot does not have its exact ABI size")
    if not load_address <= session_start < session_end <= load_address + len(image):
        fail("RAM-session slot lies outside the RAM image")
    offset = session_start - load_address
    template = image[offset : offset + RAM_SESSION_BYTES]
    if template != bytes(RAM_SESSION_BYTES):
        fail("canonical RAM-session slot is not all zero")
    _verify_session_dtb(tree)
    return {
        "offset": offset,
        "bytes": RAM_SESSION_BYTES,
        "template_sha256": sha256_bytes(template),
    }


def build_bootstrap(
    sources: dict[str, Any],
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    work: Path,
    zimage: Path,
    dtb: Path,
) -> tuple[Path, Path, dict[str, int | str]]:
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
        require_directory(target_source(target, "bootstrap")),
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
    personalization = verify_images(
        ramboot,
        zimage,
        dtb,
        ramboot_map,
        target_bootstrap["load_address"],
        target_bootstrap["payload_limit"],
        target_config["linux"]["forbidden_dtb_markers"],
    )
    return ramboot, ramboot_map, personalization


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
            log_message(f"{expected}  {destination}")
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
    make_arguments = ["LIBS=-static -lusb-1.0"] if recipe["link"] == "static-libusb" else []
    run(["make", "-C", str(source), "clean", "all", *make_arguments])
    built = require_file(source / recipe["binary"])
    destination: Path = output / recipe["name"]
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(built, destination)
    destination.chmod(0o755)
    return destination


def _verify_static_host_binary(path: Path) -> None:
    """Require a portable static Linux/x86-64 ELF with no shared dependencies."""
    data = require_file(path).read_bytes()
    if (
        len(data) < 64
        or data[:4] != b"\x7fELF"
        or data[4] != 2
        or data[5] != 1
        or struct.unpack_from("<H", data, 18)[0] != 62
    ):
        fail(f"host tool is not a little-endian Linux/x86-64 ELF: {path}")
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    program_size = struct.unpack_from("<H", data, 54)[0]
    program_count = struct.unpack_from("<H", data, 56)[0]
    if program_size < 4 or program_offset + program_size * program_count > len(data):
        fail(f"host tool has an invalid ELF program-header table: {path}")
    for index in range(program_count):
        offset = program_offset + index * program_size
        if struct.unpack_from("<I", data, offset)[0] == 3:
            fail(f"host tool must not require a dynamic ELF interpreter: {path}")

    dynamic = subprocess.run(
        ["readelf", "-dW", str(path)],
        capture_output=True,
        text=True,
        env=build_environment(),
        check=False,
    )
    if dynamic.returncode != 0:
        fail(dynamic.stderr.strip() or f"cannot inspect host ELF dependencies: {path}")
    if "(NEEDED)" in dynamic.stdout:
        fail(f"host tool must not have DT_NEEDED dependencies: {path}")


def build_cc_libusb_tool(recipe: dict[str, Any], output: Path) -> Path:
    """Build one portable static C/libusb host-tool recipe."""
    pkg = subprocess.run(
        ["pkg-config", "--cflags", "--static", "--libs", "libusb-1.0"],
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
            "-static",
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
    """Build every typed platform host-tool recipe."""
    source_work = work / "host-build"
    output = work / "host"
    source_work.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    result: dict[str, Path] = {}
    for recipe in platform["host"]["tools"]:
        if recipe["type"] == "make-archive":
            built = build_make_host_tool(sources, recipe, source_work, output)
        elif recipe["type"] == "cc-libusb":
            built = build_cc_libusb_tool(recipe, output)
        else:
            fail(f"unsupported host recipe type: {recipe['type']}")
        _verify_static_host_binary(built)
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
    release: Path,
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    image: str,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
    personalization: dict[str, int | str],
) -> dict[str, Any]:
    """Create the generic runtime contract consumed by the common runner."""
    declared_assets = {
        role: f"assets/{relative}" for role, (relative, _digest) in asset_outputs.items()
    }

    platform_host = platform["host"]
    runtime = target_config["runtime"]
    runtime_tools = {role: f"host/{name}" for role, name in platform_host["runtime_tools"].items()}
    for name in platform_host["runtime_tools"].values():
        if name not in host_tools:
            fail(f"runtime host tool was not built: {name}")
    hashes = {
        image: sha256_file(require_file(release / image)),
        "runner/platform_adapter.py": sha256_file(
            require_file(release / "runner/platform_adapter.py")
        ),
    }
    hashes["runner/ssh_transport.py"] = sha256_file(
        require_file(release / "runner/ssh_transport.py")
    )
    hashes.update(
        {
            declared_assets[role]: sha256_file(require_file(release / declared_assets[role]))
            for role in asset_outputs
        }
    )
    hashes.update(
        {
            runtime_tools[role]: sha256_file(require_file(release / runtime_tools[role]))
            for role in platform_host["runtime_tools"]
        }
    )
    return {
        "target": target,
        "display_name": target_config["display_name"],
        "platform": target_config["platform"],
        "image": image,
        "addresses": {
            "fdl1": runtime["fdl1_load_address"],
            "payload": target_config["bootstrap"]["load_address"],
        },
        "usb": runtime["usb"],
        "personalization": personalization,
        "assets": declared_assets,
        "adapter": runtime["adapter"],
        "host_tools": runtime_tools,
        "sha256": hashes,
    }


def _publish_staged_bundle(
    release: Path,
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
    personalization: dict[str, int | str],
    asset_lock_path: Path,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
    linux_recipe: str,
    device_identity: str,
    rootfs_output: Path,
    rootfs_recipe: str,
    kbuild_receipt: dict[str, str],
    bundle_packages: tuple[str, ...],
    bundle_apks: dict[str, Path],
) -> Path:
    """Complete one already-private immutable bundle staging directory."""
    if set(bundle_packages) != set(bundle_apks):
        fail("published bundle APKs differ from the declared bundle package set")
    expected_apk_files = {f"apks/{package}.apk" for package in bundle_packages}
    manifest_apk_files = {
        relative
        for relative in release_manifest["bundle_files"]
        if relative.startswith("apks/") and relative.endswith(".apk")
    }
    if manifest_apk_files != expected_apk_files:
        fail("release manifest APK files differ from the declared bundle package set")

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
    copy_file(ssh_transport_source(), release / "runner/ssh_transport.py")
    copy_file(
        adapter_source(target_config["platform"]),
        release / "runner/platform_adapter.py",
    )
    copy_file(asset_lock_path, release / "assets.lock.toml")
    copy_file(ROOT / "THIRD_PARTY_NOTICES.md", release / "THIRD_PARTY_NOTICES.md")
    for package, source in sorted(bundle_apks.items()):
        copy_file(source, release / "apks" / f"{package}.apk")

    runtime = runtime_manifest(
        release,
        target,
        target_config,
        platform,
        image_name,
        asset_outputs,
        host_tools,
        personalization,
    )
    write_json(release / "runtime-manifest.json", runtime, prefix=".runtime-manifest.")

    for relative in release_manifest["bundle_files"]:
        require_file(release / relative)

    workspace_digest = os.environ.get("FPLINUX_WORKSPACE_DIGEST", "")
    container_image_recipe = os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE", "")
    require_sha256(workspace_digest, "workspace digest")
    require_sha256(container_image_recipe, "container image recipe")
    require_sha256(linux_recipe, "Linux recipe")
    require_sha256(device_identity, "device identity")
    apk_signing_key = require_sha256(
        alpine_state.signing_key_identity(CACHE), "APK signing public key"
    )
    rootfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
    if not isinstance(kbuild_receipt, dict) or set(kbuild_receipt) != {"recipe", "sha256"}:
        fail("Kbuild receipt identity is invalid")
    kbuild_receipt = {
        "recipe": require_sha256(kbuild_receipt.get("recipe"), "Kbuild recipe"),
        "sha256": require_sha256(kbuild_receipt.get("sha256"), "Kbuild receipt SHA-256"),
    }
    payload = {
        "target": target,
        "workspace_digest": workspace_digest,
        "container_image_recipe": container_image_recipe,
        "apk_signing_key": apk_signing_key,
        "linux_recipe": linux_recipe,
        "device_identity": device_identity,
        "rootfs_receipt": rootfs_receipt,
        "kbuild_receipt": kbuild_receipt,
        "files": published_file_records(release),
    }
    generation = sha256_bytes(canonical_json_bytes(payload))
    manifest = {**payload, "generation": generation}
    write_json(release / "build-manifest.json", manifest, prefix=".build-manifest.")
    generation_path = publish_bundle_generation(
        OUTPUT,
        target,
        release,
        generation,
    )
    publish_current_bundle(OUTPUT, target, generation_path)
    return generation_path


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
    personalization: dict[str, int | str],
    asset_lock_path: Path,
    asset_outputs: dict[str, tuple[str, str]],
    host_tools: dict[str, Path],
    linux_recipe: str,
    device_identity: str,
    rootfs_output: Path,
    rootfs_recipe: str,
    kbuild_receipt: dict[str, str],
    bundle_packages: tuple[str, ...],
    bundle_apks: dict[str, Path],
) -> Path:
    """Publish a complete immutable bundle and select it as current."""
    release = create_bundle_staging(OUTPUT, target)
    try:
        return _publish_staged_bundle(
            release,
            target,
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
            personalization,
            asset_lock_path,
            asset_outputs,
            host_tools,
            linux_recipe,
            device_identity,
            rootfs_output,
            rootfs_recipe,
            kbuild_receipt,
            bundle_packages,
            bundle_apks,
        )
    finally:
        discard_bundle_staging(OUTPUT, target, release)


def main() -> None:
    """Build stages 1-4 and publish one deterministic target bundle."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--jobs", type=int, required=True)
    args = parser.parse_args()
    if args.jobs < 1:
        fail("jobs must be positive")

    reporter = RunReporter.from_environment(f"build {args.target}", "build")
    with report_stage(reporter, "configuration"):
        target_config = load_target(args.target)
        platform = load_platform(target_config["platform"])
        rootfs_packages = alpine_state.selected_packages(platform, target_config)
        bundle_packages = alpine_state.bundle_packages(platform, target_config, rootfs_packages)
        with (ROOT / "sources.lock.toml").open("rb") as stream:
            sources = tomllib.load(stream)
        linux_base = require_sha256(
            source_lock_entry(sources, platform["linux"]["source_lock"]).get("sha256"),
            "Linux source",
        )
        asset_lock_path = require_file(target_asset_lock_path(args.target))
        release_manifest = load_release(args.target)

        work = OUTPUT / args.target / "work"
        work.mkdir(parents=True, exist_ok=True)
        CACHE.mkdir(parents=True, exist_ok=True)

    with report_stage(reporter, "prepare-linux"):
        linux_source, prepared_linux = prepare_linux(
            sources,
            args.target,
            target_config,
            platform,
        )

    with report_stage(reporter, "rootfs"):
        rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = alpine_builder.build_rootfs(
            args.jobs, rootfs_packages, bundle_packages
        )
    cross = platform["linux"]["cross_compile"]
    kernel_output = work / "kernel"
    with report_stage(reporter, "kernel"):
        bootstrap_recipe = bootstrap_recipe_digest(
            sources,
            args.target,
            target_config,
            platform,
        )
        zimage, dtb, kbuild_receipt, device_identity = build_kernel(
            args.target,
            target_config,
            platform,
            bootstrap_recipe,
            linux_source,
            prepared_linux,
            linux_base,
            kernel_output,
            cross,
            rootfs,
            rootfs_output,
            rootfs_recipe,
            args.jobs,
        )
    with report_stage(reporter, "bootstrap"):
        ramboot, ramboot_map, personalization = build_bootstrap(
            sources,
            args.target,
            target_config,
            platform,
            work,
            zimage,
            dtb,
        )
    with report_stage(reporter, "assets"):
        asset_outputs = build_assets(asset_lock_path, work / "assets")
    with report_stage(reporter, "host-tools"):
        host_tools = build_host_tools(sources, platform, work)
    with report_stage(reporter, "publish"):
        publish_bundle(
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
            personalization,
            asset_lock_path,
            asset_outputs,
            host_tools,
            prepared_linux.linux_recipe,
            device_identity,
            rootfs_output,
            rootfs_recipe,
            kbuild_receipt,
            bundle_packages,
            bundle_apk_outputs,
        )


if __name__ == "__main__":
    run_entrypoint(main)

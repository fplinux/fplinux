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

from . import alpine_builder, alpine_state, kbuild_state, linux_state, profile_layout
from .build_env import build_environment
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
    PROFILE_HOST_PLUGIN_BUNDLE_PATH,
    container_runtime_recipe_digest,
    load_asset_lock,
    load_platform,
    load_release,
    load_target,
    relative_value,
    target_asset_lock_path,
    target_defconfig_path,
)
from .device_state import DeviceStateError, device_kernel_identity, localversion
from .device_tree import (
    DeviceTreeError,
    exact_path_properties,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    verify_target_identity,
)
from .identity import (
    RUNTIME_IDENTITY_PATH,
    IdentityError,
)
from .identity_codegen import (
    BOOTSTRAP_IDENTITY_HEADER,
    LINUX_IDENTITY_DTSI,
    LINUX_PLATFORM_IDENTITY_HEADER,
    bootstrap_identity_header,
    linux_identity_dtsi,
    linux_machine_binding,
    linux_machine_binding_path,
    linux_platform_identity_header,
    runtime_identity,
)
from .kbuild_state import KbuildStateError
from .linux_state import LinuxStateError, PreparedLinuxState
from .output import RunReporter, current_stage, run_entrypoint

if TYPE_CHECKING:
    from collections.abc import Iterator

    from .uboot_tools import UbootBuild

CACHE = Path("/cache")
OUTPUT = Path("/out")
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


def container_image_environment() -> tuple[str, str, str]:
    """Validate the static image recipe, exact generation and derived runtime recipe."""
    image_recipe = require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE", ""),
        "container image recipe",
    )
    generation = require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_GENERATION", ""),
        "container image generation",
    )
    runtime_recipe = require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE", ""),
        "container runtime recipe",
    )
    if runtime_recipe != container_runtime_recipe_digest(image_recipe, generation):
        fail("container runtime recipe does not match its image recipe and generation")
    return image_recipe, generation, runtime_recipe


def root_source(relative: str) -> Path:
    """Resolve a repository-relative source file or directory."""
    return ROOT / relative_value(relative, "repository source path")


def target_source(target: str, relative: str) -> Path:
    """Resolve a target-relative source file or directory."""
    return ROOT / "targets" / target / relative_value(relative, "target source path")


def selected_profile(target_config: dict[str, Any]) -> str | None:
    """Return the optional profile identity already validated by target loading."""
    profile = target_config.get("profile")
    if profile is None:
        return None
    if not isinstance(profile, str) or not profile:
        fail("target profile is invalid")
    return profile


def profile_kconfig_actions(target_config: dict[str, Any]) -> tuple[list[str], list[str]]:
    """Return the profile-only scripts/config actions in declared order."""
    linux = target_config["linux"]
    enable = linux.get("config_enable", [])
    disable = linux.get("config_disable", [])
    if (
        not isinstance(enable, list)
        or not isinstance(disable, list)
        or not all(isinstance(symbol, str) and symbol for symbol in [*enable, *disable])
    ):
        fail("target profile Kconfig actions are invalid")
    return list(enable), list(disable)


def profile_kconfig_arguments(config_enable: list[str], config_disable: list[str]) -> list[str]:
    """Render declared CONFIG_* actions for the Linux scripts/config command."""
    arguments: list[str] = []
    for action, symbols in (("--enable", config_enable), ("--disable", config_disable)):
        for symbol in symbols:
            if not symbol.startswith("CONFIG_") or len(symbol) == len("CONFIG_"):
                fail("target profile Kconfig symbol is invalid")
            arguments.extend((action, symbol.removeprefix("CONFIG_")))
    return arguments


def assert_profile_kconfig(
    config: Path, config_enable: list[str], config_disable: list[str]
) -> None:
    """Require selected profile Kconfig values after dependency resolution."""
    text = require_file(config).read_text()
    for symbol in config_enable:
        if f"{symbol}=y\n" not in text:
            fail(f"profile did not enable {symbol}")
    for symbol in config_disable:
        if f"# {symbol} is not set\n" not in text:
            fail(f"profile did not disable {symbol}")


def runner_source() -> Path:
    """Return the one shared runner source path."""
    return ROOT / "common/run.py"


def ssh_transport_source() -> Path:
    """Return the SSH session helper published beside the runner."""
    return ROOT / "scripts/fplinux_cli/ssh_transport.py"


def identity_source() -> Path:
    """Return the shared identity contract published beside the runner."""
    return ROOT / "scripts/fplinux_cli/identity.py"


def adapter_source(platform: str) -> Path:
    """Return the fixed adapter path for a validated platform."""
    return ROOT / "platforms" / platform / "host/adapter.py"


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


PROFILE_ROOT_DTSI = "arch/arm/boot/dts/unisoc/fplinux-external-root.dtsi"


def generated_linux_files(
    target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, bytes]:
    """Return exact generated Linux files keyed by destination."""
    target_identity = target_config["identity"]
    platform_identity = platform["identity"]
    files = {
        LINUX_IDENTITY_DTSI: linux_identity_dtsi(target_identity, platform_identity),
        LINUX_PLATFORM_IDENTITY_HEADER: linux_platform_identity_header(platform_identity),
        linux_machine_binding_path(target_identity): linux_machine_binding(
            target_identity, platform_identity
        ),
    }
    root = target_config["linux"]["root"]
    if root["kind"] == "external":
        files[PROFILE_ROOT_DTSI] = profile_layout.external_root_dtsi(root)
    return files


def write_generated_files(root: Path, files: dict[str, bytes], *, owner: str) -> None:
    """Write generated files into one private projection without replacing source."""
    for relative, contents in sorted(files.items()):
        destination = root / relative_value(relative, f"{owner} generated path")
        if destination.is_symlink() or destination.exists():
            fail(f"{owner} generated path collides with projected source: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(contents)


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
        "generated": [
            {"destination": destination, "sha256": sha256_bytes(contents)}
            for destination, contents in sorted(
                generated_linux_files(target_config, platform).items()
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


def generated_bootstrap_identity(target_config: dict[str, Any]) -> bytes:
    """Return the exact target identity header consumed by bootstrap C."""
    try:
        return bootstrap_identity_header(
            target_config["identity"], target_config["bootstrap"]["record_prefix"]
        )
    except IdentityError as error:
        fail(str(error))
    return b""


def effective_boot_layout(
    target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, int]:
    """Return the one layout selected for this bootstrap build."""
    layout = target_config.get("layout")
    return layout if isinstance(layout, dict) else platform["bootstrap"]["layout"]


def generated_bootstrap_files(
    target_config: dict[str, Any], platform: dict[str, Any]
) -> dict[str, bytes]:
    """Return every transient input generated for one bootstrap build."""
    layout = effective_boot_layout(target_config, platform)
    return {
        BOOTSTRAP_IDENTITY_HEADER: generated_bootstrap_identity(target_config),
        "generated/fplinux-boot-layout.h": profile_layout.boot_layout_header(layout),
        "generated/fplinux-bootstrap-memory.ld": profile_layout.bootstrap_memory_ld(
            target_config["bootstrap"], layout
        ),
    }


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
    generated = {
        path: sha256_bytes(contents)
        for path, contents in generated_bootstrap_files(target_config, platform).items()
    }
    manifest = {
        "target": target,
        "target_bootstrap": target_bootstrap,
        "platform_bootstrap": platform_bootstrap,
        "target_source": bootstrap_tree_entries(target_source(target, target_bootstrap["source"])),
        "shared_copies": shared_copies,
        "vendor_source": {
            "commit": vendor_commit,
            "archive_sha256": require_sha256(
                vendor_source.get("archive_sha256"),
                "bootstrap vendor source",
            ),
        },
        "generated": generated,
        # build_bootstrap() is in this file, already part of the Kbuild plan.
        "implementation": {
            "scripts/fplinux_cli/build_env.py": sha256_file(
                root_source("scripts/fplinux_cli/build_env.py")
            ),
            "scripts/fplinux_cli/builder.py": sha256_file(
                root_source("scripts/fplinux_cli/builder.py")
            ),
        },
    }
    return sha256_bytes(canonical_json_bytes(manifest))


def apply_patches(source: Path, paths: list[Path]) -> None:
    """Apply ordered, fuzz-free patches to one verified source projection."""
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


def profile_linux_source_path(parent: Path, target: str, profile: str) -> Path:
    """Create a profile-only prepared-Linux slot beside, never inside, the default tree."""
    root = require_directory(parent.parent)
    source = root
    for component in ("profiles", target, profile):
        source /= component
        if source.exists():
            require_directory(source)
        else:
            source.mkdir()
    return require_directory(source)


def discard_profile_linux_source(parent: Path, target: str, profile: str) -> None:
    """Discard a stale dedicated source tree after a profile now shares default sources."""
    root = require_directory(parent.parent)
    profiles = root / "profiles"
    if profiles.is_symlink():
        fail(f"profile Linux source root must not be a symlink: {profiles}")
    if not profiles.exists():
        return
    require_directory(profiles)
    target_slot = profiles / target
    if target_slot.is_symlink():
        fail(f"profile Linux target slot must not be a symlink: {target_slot}")
    if not target_slot.exists():
        return
    require_directory(target_slot)
    source = target_slot / profile
    if source.is_symlink():
        fail(f"profile Linux source slot must not be a symlink: {source}")
    if not source.exists():
        return
    if not source.is_dir():
        fail(f"profile Linux source slot is invalid: {source}")
    shutil.rmtree(source)


def prepared_linux_staging_path(parent: Path, target: str, profile: str | None) -> Path:
    """Create one empty, bounded staging slot for a default or named profile source tree."""
    root = require_directory(parent.parent)
    staging = root
    components: tuple[str, ...]
    if profile is None:
        components = ("staging", target, "default")
    else:
        components = ("staging", target, "profiles", profile)
    for component in components:
        staging /= component
        if staging.is_symlink():
            fail(f"prepared Linux staging slot must not be a symlink: {staging}")
        if staging.exists():
            if not staging.is_dir():
                fail(f"prepared Linux staging slot is invalid: {staging}")
        else:
            staging.mkdir()
    if staging.is_symlink() or not staging.is_dir():
        fail(f"prepared Linux staging slot is invalid: {staging}")
    shutil.rmtree(staging)
    staging.mkdir()
    return staging


def discard_prepared_linux_staging(staging: Path) -> None:
    """Discard only one real staging slot after publish or a failed preparation."""
    if staging.is_symlink():
        fail(f"prepared Linux staging slot must not be a symlink: {staging}")
    if not staging.exists():
        return
    if not staging.is_dir():
        fail(f"prepared Linux staging slot is invalid: {staging}")
    shutil.rmtree(staging)


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
    generated_files = generated_linux_files(target_config, platform)
    try:
        parent = linux_state.ensure_sources_directory(CACHE)
    except LinuxStateError as error:
        fail(str(error))
    source = parent / target
    profile = selected_profile(target_config)
    if profile is not None:
        default_config = load_target(target)
        default_recipe = linux_recipe_digest(linux, target, default_config, platform)
        if default_recipe != recipe:
            source = profile_linux_source_path(parent, target, profile)
        else:
            discard_profile_linux_source(parent, target, profile)

    def apply_projection(destination: Path) -> None:
        apply_patches(destination, platform_patches)
        copy_steps(destination, copies)
        apply_patches(destination, target_patches)
        append_steps(destination, appends)
        write_generated_files(destination, generated_files, owner="Linux projection")

    prepared = linux_state.inspect_prepared_linux(source, recipe)
    if prepared is not None:
        return source, prepared

    archive = fetch(
        linux.get("url"),
        source_digest,
        CACHE / "downloads/linux",
        f"linux-{version}.tar.xz",
    )
    staging = prepared_linux_staging_path(parent, target, profile)
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
        discard_prepared_linux_staging(staging)
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
        root_contract = target_config["linux"]["root"]
        initramfs_record: dict[str, int | str] | None = None
        initramfs_input: Path | None = None
        initramfs_receipt: dict[str, str] | None = None
        if root_contract["kind"] == "initramfs":
            initramfs_record = kbuild_state.initramfs_identity(rootfs)
            initramfs_input = kbuild_state.initramfs_input_path(work, initramfs_record)
            initramfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
            device_root: dict[str, object] = {
                "kind": "initramfs",
                "artifact": initramfs_record,
                "receipt": initramfs_receipt,
            }
        elif root_contract["kind"] == "external":
            device_root = root_contract
        else:
            fail(f"unsupported Linux root kind: {root_contract['kind']}")
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
            (
                "scripts/fplinux_cli/build_env.py",
                root_source("scripts/fplinux_cli/build_env.py"),
            ),
            (
                "scripts/fplinux_cli/builder.py",
                root_source("scripts/fplinux_cli/builder.py"),
            ),
            (
                "scripts/fplinux_cli/device_state.py",
                Path(__file__).with_name("device_state.py"),
            ),
            ("scripts/fplinux_cli/kbuild_state.py", Path(kbuild_state.__file__)),
        ]
        config_enable, config_disable = profile_kconfig_actions(target_config)
        device_identity = device_kernel_identity(
            target=target,
            linux_recipe=current_linux.linux_recipe,
            bootstrap_recipe=bootstrap_recipe,
            root=device_root,
            kbuild_implementation=kbuild_state.implementation_identity(implementation),
            arch=platform["linux"]["arch"],
            defconfig=defconfig,
            dtb=target_config["linux"]["dtb"],
            profile=selected_profile(target_config),
            config_enable=config_enable,
            config_disable=config_disable,
        )
        initramfs_source = str(initramfs_input) if initramfs_input is not None else ""
        config_command = [
            str(config_script),
            "--file",
            str(output / ".config"),
            "--set-str",
            "INITRAMFS_SOURCE",
            initramfs_source,
            "--set-str",
            "LOCALVERSION",
            localversion(device_identity),
            *profile_kconfig_arguments(config_enable, config_disable),
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
            root=root_contract,
            initramfs=initramfs_record,
            initramfs_input=initramfs_input,
            initramfs_receipt=initramfs_receipt,
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
            if initramfs_record is not None:
                kbuild_state.materialize_initramfs_input(work, rootfs, plan)
            shutil.copyfile(defconfig, output / ".config")
            for command in commands[:3]:
                run(command)
            assert_profile_kconfig(output / ".config", config_enable, config_disable)
            run(commands[3])

        zimage = require_file(output / platform["linux"]["image_output"])
        dtb = require_file(
            output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
        )
        try:
            verify_target_identity(
                dtb,
                target,
                target_config["identity"]["display_name"],
                (
                    target_config["identity"]["compatible"],
                    platform["identity"]["compatible"],
                ),
            )
            verify_root_bootargs(dtb, root_contract)
            layout = target_config.get("layout")
            if isinstance(layout, dict):
                verify_profile_dtb_layout(dtb, layout)
        except DeviceTreeError as error:
            fail(str(error))
        config_text = require_file(output / ".config").read_text()
        assert_profile_kconfig(output / ".config", config_enable, config_disable)
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


def _verify_session_dtb(tree: bytes) -> None:
    """Require one canonical marker for each RAM-session DT property."""
    try:
        properties = exact_path_properties(tree, ("/chosen", "/fplinux-session"))
    except DeviceTreeError as error:
        fail(str(error))
    chosen = properties["/chosen"]
    session = properties["/fplinux-session"]
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


def uboot_build_header(uboot: UbootBuild) -> bytes:
    """Generate exact stage0 constants from the verified U-Boot artifact."""
    license_tag = "SPDX-License-" + "Identifier"
    return (
        f"/* {license_tag}: GPL-2.0-only */\n"
        "/* Generated from the selected full U-Boot artifact. */\n"
        "#ifndef FPLINUX_UBOOT_BUILD_H\n"
        "#define FPLINUX_UBOOT_BUILD_H\n\n"
        f"#define FPLINUX_UBOOT_ENTRY_PHYS 0x{uboot.entry:08x}U\n"
        f"#define FPLINUX_UBOOT_BINARY_BYTES {uboot.binary.stat().st_size}U\n"
        "\n"
        "#endif\n"
    ).encode("ascii")


def _bootstrap_map_symbols(map_file: Path) -> dict[str, int]:
    symbols: dict[str, int] = {}
    for line in map_file.read_text().splitlines():
        fields = line.split()
        if len(fields) < 3:
            continue
        try:
            symbols[fields[2]] = int(fields[0], 16)
        except ValueError:
            continue
    return symbols


def verify_sd_stage0_image(
    ramboot: Path,
    uboot: UbootBuild,
    map_file: Path,
    load_address: int,
    payload_limit: int,
    layout: dict[str, int],
) -> dict[str, int | str]:
    """Verify the resident stage0, embedded U-Boot and session slot."""
    image = ramboot.read_bytes()
    binary = uboot.binary.read_bytes()
    if image[:4] != b"DHTB" or len(image) < 0x200:
        fail("U-Boot stage0 output does not have a complete DHTB header")
    declared = struct.unpack_from("<I", image, 0x30)[0]
    if declared + 0x200 > len(image):
        fail("U-Boot stage0 DHTB declared size exceeds the RAM image")
    if load_address + len(image) >= payload_limit:
        fail("U-Boot stage0 reaches its declared payload limit")

    symbols = _bootstrap_map_symbols(map_file)
    required = {
        "__image_start",
        "__bss_end",
        "uboot_payload_start",
        "uboot_payload_end",
        "fplinux_session_start",
        "fplinux_session_end",
        "stage0_ops",
        "uboot_handoff",
        "FPLINUX_BOOTSTRAP_STORAGE_DISABLED",
    }
    missing = sorted(required - symbols.keys())
    if missing:
        fail(f"U-Boot stage0 map lacks symbols: {', '.join(missing)}")
    if symbols["FPLINUX_BOOTSTRAP_STORAGE_DISABLED"] != 1:
        fail("U-Boot stage0 storage-disabled marker is not one")
    if symbols["__image_start"] != load_address:
        fail("U-Boot stage0 map image start differs from target load_address")
    if symbols["__bss_end"] > layout["uboot_stack"]:
        fail("resident U-Boot stage0 overlaps the full U-Boot stack")

    uboot_start = symbols["uboot_payload_start"]
    uboot_end = symbols["uboot_payload_end"]
    if not load_address <= uboot_start < uboot_end <= load_address + len(image):
        fail("embedded full U-Boot range lies outside stage0")
    if uboot_end - uboot_start != len(binary):
        fail("embedded full U-Boot size differs from its verified binary")
    if image[uboot_start - load_address : uboot_end - load_address] != binary:
        fail("embedded full U-Boot bytes differ from its verified binary")

    session_start = symbols["fplinux_session_start"]
    session_end = symbols["fplinux_session_end"]
    expected_start = (uboot_end + RAM_SESSION_ALIGNMENT - 1) & -RAM_SESSION_ALIGNMENT
    if session_start != expected_start or session_start % RAM_SESSION_ALIGNMENT:
        fail("stage0 session slot does not immediately follow embedded U-Boot")
    if session_end - session_start != RAM_SESSION_BYTES:
        fail("stage0 session slot does not have its exact ABI size")
    offset = session_start - load_address
    template = image[offset : offset + RAM_SESSION_BYTES]
    if template != bytes(RAM_SESSION_BYTES):
        fail("canonical stage0 session slot is not all zero")
    for name in ("stage0_ops", "uboot_handoff"):
        if not load_address <= symbols[name] < symbols["__bss_end"]:
            fail(f"resident {name} lies outside the stage0 image")
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
    uboot: UbootBuild | None = None,
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
    write_generated_files(
        bootstrap,
        generated_bootstrap_files(target_config, platform),
        owner="bootstrap generated inputs",
    )
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("U-Boot stage0 requires a verified full U-Boot artifact")
        write_generated_files(
            bootstrap,
            {"generated/fplinux-uboot-build.h": uboot_build_header(uboot)},
            owner="U-Boot stage0",
        )

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
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("U-Boot stage0 lost its verified full U-Boot artifact")
        shutil.copyfile(uboot.binary, projected_output / "u-boot.bin")
    else:
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
    if target_bootstrap["kind"] == "uboot-stage0":
        if uboot is None:
            fail("U-Boot stage0 lost its verified full U-Boot artifact")
        personalization = verify_sd_stage0_image(
            ramboot,
            uboot,
            ramboot_map,
            target_bootstrap["load_address"],
            target_bootstrap["payload_limit"],
            target_config["layout"],
        )
    else:
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


def copy_host_project_files(source: Path, copies: list[dict[str, str]]) -> None:
    """Copy declared project-owned host inputs beside verified upstream sources."""
    for step in copies:
        destination = source / relative_value(step["destination"], "host copy destination")
        if destination.exists() or destination.is_symlink():
            fail(f"host copy destination collides with verified source: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(require_file(root_source(step["source"])), destination)


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
    with tempfile.TemporaryDirectory(
        dir=work,
        prefix=f".{recipe['source_directory']}.",
    ) as temporary:
        source = Path(temporary) / recipe["source_directory"]
        extract_host_members(archive, prefix, recipe["members"], hashes, source)
        copy_host_project_files(source, recipe["copies"])
        apply_patches(source, [root_source(path) for path in recipe["patches"]])
        make_arguments = ["LIBS=-static -lusb-1.0"] if recipe["link"] == "static-libusb" else []
        run(["make", "-C", str(source), "clean", "all", *make_arguments])
        built = require_file(source / recipe["binary"])
        if recipe["self_test"]:
            run([str(built), "--self-test"])
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


def build_profile_uboot(
    target: str, target_config: dict[str, Any], work: Path, jobs: int
) -> UbootBuild | None:
    """Build the full U-Boot selected by one profile."""
    config = target_config["uboot"]
    if config["kind"] == "none":
        return None
    if config["kind"] != "full":
        fail(f"unsupported U-Boot profile kind: {config['kind']}")
    lock = config["lock"]
    archive = fetch(
        lock["archive_url"],
        lock["archive_sha256"],
        CACHE / "downloads/uboot",
        "source.tar.bz2",
    )
    container_recipe = require_sha256(
        os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE"),
        "container image recipe",
    )
    from . import uboot_tools  # noqa: PLC0415 -- profile-only source.

    try:
        profile = selected_profile(target_config)
        if profile is None:
            fail("full U-Boot requires a selected profile")
        profile_root = ROOT / "targets" / target / "profiles" / profile
        projections = [
            (require_file(ROOT / step["source"]), step["destination"]) for step in config["copies"]
        ]
        work.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work, prefix=".uboot-inputs.") as name:
            generated = Path(name)
            defconfig = generated / "ta1618_defconfig"
            layout_header = generated / "fplinux-boot-layout.h"
            layout_dtsi = generated / "fplinux-uboot-layout.dtsi"
            defconfig.write_bytes(
                profile_layout.uboot_defconfig(
                    require_file(profile_root / config["defconfig"]).read_bytes(),
                    target_config["layout"],
                )
            )
            layout_header.write_bytes(profile_layout.boot_layout_header(target_config["layout"]))
            layout_dtsi.write_bytes(profile_layout.uboot_layout_dtsi(target_config["layout"]))
            projections.append((layout_header, "include/fplinux-boot-layout.h"))
            projections.append((layout_dtsi, "arch/arm/dts/fplinux-uboot-layout.dtsi"))
            uboot = uboot_tools.build_full(
                archive,
                config,
                defconfig,
                projections,
                [require_file(profile_root / path) for path in config["patches"]],
                work,
                jobs,
                container_recipe,
                "arm-none-eabi-",
                target_config["layout"],
            )
    except (
        OSError,
        subprocess.SubprocessError,
        tarfile.TarError,
        uboot_tools.UbootToolsError,
    ) as error:
        fail(str(error))
    log_message(f"U-Boot build ready: {uboot.receipt['recipe'][:16]}")
    return uboot


def profile_ext4_artifact(
    target_config: dict[str, Any],
    work: Path,
    rootfs_output: Path,
    rootfs_recipe: str,
) -> Path | None:
    """Recheck and return the selected ext4 artifact."""
    config = target_config["image"]
    if config["kind"] == "none":
        return None
    if config["kind"] != "ext4-root":
        fail(f"unsupported profile image kind: {config['kind']}")
    from . import ext4_root  # noqa: PLC0415 -- profile-only source.

    try:
        rootfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
        plan = ext4_root.create_plan(
            config,
            rootfs_recipe,
            rootfs_receipt,
            require_sha256(
                os.environ.get("FPLINUX_CONTAINER_IMAGE_RECIPE"),
                "container image recipe",
            ),
        )
        output = work / "rootfs-image"
        ext4_root.receipt_identity(output, plan)
    except (OSError, ext4_root.Ext4RootError) as error:
        fail(str(error))
    return require_file(output / config["filename"])


def build_profile_fit(
    target: str,
    target_config: dict[str, Any],
    work: Path,
    zimage: Path,
    dtb: Path,
    uboot: UbootBuild | None,
) -> Path | None:
    """Build and recheck the native FIT selected by one profile."""
    config = target_config["fit"]
    if config["kind"] == "none":
        return None
    if config["kind"] != "sha256" or uboot is None:
        fail("SHA-256 FIT requires verified U-Boot tools")
    from . import fit_image  # noqa: PLC0415 -- profile-only source.

    try:
        plan = fit_image.create_plan(
            target,
            target_config["identity"]["display_name"],
            config,
            zimage,
            dtb,
            uboot.receipt,
        )
        output = work / "fit"
        fit = fit_image.build(
            uboot.mkimage,
            uboot.dumpimage,
            zimage,
            dtb,
            output,
            plan,
        )
        fit_image.receipt_identity(output, plan)
    except (
        OSError,
        subprocess.SubprocessError,
        DeviceTreeError,
        fit_image.FitImageError,
    ) as error:
        fail(str(error))
    return require_file(fit)


def build_profile_sd_image(
    target_config: dict[str, Any],
    work: Path,
    fit: Path | None,
    ext4: Path | None,
) -> Path | None:
    """Assemble the selected whole-card image from verified profile artifacts."""
    image_config = target_config["image"]
    if image_config["kind"] == "none":
        return None
    if image_config["kind"] != "ext4-root" or fit is None or ext4 is None:
        fail("whole-card image requires FIT and ext4 root artifacts")
    storage = target_config["storage"]
    if not isinstance(storage, dict):
        fail("whole-card image requires a storage layout")
    from . import sd_image  # noqa: PLC0415 -- profile-only source.

    destination = work / "sd-image" / sd_image.compressed_image_name(storage)
    try:
        return require_file(
            sd_image.build(
                fit,
                ext4,
                destination,
                fit_spec=target_config["fit"],
                storage=storage,
            )
        )
    except (OSError, subprocess.SubprocessError, sd_image.SdImageError) as error:
        fail(str(error))


def profile_boot_artifact_set(
    target_config: dict[str, Any],
    disk_image: Path | None,
) -> tuple[dict[str, Path], dict[str, Any]]:
    """Collect the profile payload consumed by package and run."""
    files: dict[str, Path] = {}
    image_config = target_config["image"]
    if image_config["kind"] == "ext4-root":
        if disk_image is None:
            fail("whole-card image artifact is missing")
        image_bundle_path = disk_image.name
        files[image_bundle_path] = disk_image
        required = [image_bundle_path]
    else:
        required = []

    metadata = {
        "required": required,
        "runnable": target_config["runtime"]["runnable"],
    }
    return files, metadata


def default_boot_artifacts() -> dict[str, Any]:
    """Return the ordinary RAM pipeline contract for callers without extra artifacts."""
    return {
        "required": [],
        "runnable": True,
    }


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
    hashes[RUNTIME_IDENTITY_PATH] = sha256_file(require_file(release / RUNTIME_IDENTITY_PATH))
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
        "profile": selected_profile(target_config),
        "transport": runtime.get("transport", "usb-ncm"),
        "identity": runtime_identity(
            target_config["identity"],
            target_config["platform"],
            platform["identity"],
        ),
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
    boot_files: dict[str, Path],
    boot_artifacts: dict[str, Any],
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
    debug_outputs = [
        (zimage, "zImage"),
        (dtb, target_config["linux"]["debug_dtb"]),
        (kernel_output / "vmlinux", "vmlinux"),
        (kernel_output / "System.map", "System.map"),
        (kernel_output / ".config", "kernel.config"),
        (ramboot_map, "ramboot.map"),
    ]
    if target_config["linux"]["root"]["kind"] == "initramfs":
        debug_outputs.append((rootfs, "rootfs.cpio"))
    for source, name in debug_outputs:
        copy_file(source, release / "debug" / name)
    for relative, _digest in asset_outputs.values():
        copy_file(work / "assets" / relative, release / "assets" / relative)
    for name, source in host_tools.items():
        copy_file(source, release / "host" / name, executable=True)
    copy_file(runner_source(), release / "runner/run.py", executable=True)
    copy_file(ssh_transport_source(), release / "runner/ssh_transport.py")
    profile_plugin = target_config["runtime"].get("host_plugin")
    if isinstance(profile_plugin, str):
        copy_file(
            ROOT / "targets" / target / profile_plugin,
            release / PROFILE_HOST_PLUGIN_BUNDLE_PATH,
        )
    copy_file(identity_source(), release / RUNTIME_IDENTITY_PATH)
    copy_file(
        adapter_source(target_config["platform"]),
        release / "runner/platform_adapter.py",
    )
    copy_file(asset_lock_path, release / "assets.lock.toml")
    copy_file(ROOT / "THIRD_PARTY_NOTICES.md", release / "THIRD_PARTY_NOTICES.md")
    for package, source in sorted(bundle_apks.items()):
        copy_file(source, release / "apks" / f"{package}.apk")
    for relative, source in sorted(boot_files.items()):
        destination = release / relative_value(relative, "boot artifact path")
        if destination.exists() or destination.is_symlink():
            fail(f"boot artifact collides with a bundle file: {relative}")
        copy_file(source, destination)

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
    container_image_recipe, container_image_generation, _runtime_recipe = (
        container_image_environment()
    )
    require_sha256(workspace_digest, "workspace digest")
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
        "profile": selected_profile(target_config),
        "workspace_digest": workspace_digest,
        "container_image_recipe": container_image_recipe,
        "container_image_generation": container_image_generation,
        "apk_signing_key": apk_signing_key,
        "linux_recipe": linux_recipe,
        "device_identity": device_identity,
        "rootfs_receipt": rootfs_receipt,
        "kbuild_receipt": kbuild_receipt,
        "boot_artifacts": boot_artifacts,
        "files": published_file_records(release),
    }
    generation = sha256_bytes(canonical_json_bytes(payload))
    manifest = {**payload, "generation": generation}
    write_json(release / "build-manifest.json", manifest, prefix=".build-manifest.")
    profile = selected_profile(target_config)
    generation_path = publish_bundle_generation(
        OUTPUT,
        target,
        release,
        generation,
        profile,
    )
    publish_current_bundle(OUTPUT, target, generation_path, profile)
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
    boot_files: dict[str, Path] | None = None,
    boot_artifacts: dict[str, Any] | None = None,
) -> Path:
    """Publish a complete immutable bundle and select it as current."""
    profile = selected_profile(target_config)
    release = create_bundle_staging(OUTPUT, target, profile)
    if boot_files is None:
        boot_files = {}
    if boot_artifacts is None:
        boot_artifacts = default_boot_artifacts()
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
            boot_files,
            boot_artifacts,
        )
    finally:
        discard_bundle_staging(OUTPUT, target, release, profile)


def main() -> None:
    """Build stages 1-4 and publish one deterministic target bundle."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True)
    parser.add_argument("--profile")
    parser.add_argument("--jobs", type=int, required=True)
    args = parser.parse_args()
    if args.jobs < 1:
        fail("jobs must be positive")

    container_image_environment()

    reporter = RunReporter.from_environment(f"build {args.target}", "build")
    with report_stage(reporter, "configuration"):
        target_config = load_target(args.target, args.profile)
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

        profile = selected_profile(target_config)
        work = OUTPUT / args.target / "work"
        if profile is not None:
            work = OUTPUT / args.target / "profiles" / profile / "work"
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
        if target_config["image"]["kind"] == "ext4-root":
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = alpine_builder.build_rootfs(
                args.jobs,
                rootfs_packages,
                bundle_packages,
                external_image=target_config["image"],
                external_output=work / "rootfs-image",
            )
        else:
            rootfs, rootfs_output, rootfs_recipe, bundle_apk_outputs = alpine_builder.build_rootfs(
                args.jobs, rootfs_packages, bundle_packages
            )
        ext4_artifact = profile_ext4_artifact(
            target_config,
            work,
            rootfs_output,
            rootfs_recipe,
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
    profile_uboot = None
    if target_config["uboot"]["kind"] != "none":
        with report_stage(reporter, "uboot"):
            profile_uboot = build_profile_uboot(args.target, target_config, work, args.jobs)
    fit_artifact = None
    if target_config["fit"]["kind"] != "none":
        with report_stage(reporter, "fit"):
            fit_artifact = build_profile_fit(
                args.target,
                target_config,
                work,
                zimage,
                dtb,
                profile_uboot,
            )
    sd_image_artifact = None
    if target_config["image"]["kind"] != "none":
        with report_stage(reporter, "sd-image"):
            sd_image_artifact = build_profile_sd_image(
                target_config,
                work,
                fit_artifact,
                ext4_artifact,
            )
    boot_files, boot_artifacts = profile_boot_artifact_set(
        target_config,
        sd_image_artifact,
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
            profile_uboot,
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
            boot_files,
            boot_artifacts,
        )


if __name__ == "__main__":
    run_entrypoint(main)

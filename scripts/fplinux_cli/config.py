# SPDX-License-Identifier: GPL-2.0-only
"""Load and validate targets, platforms and the build environment lock."""

from __future__ import annotations

import hashlib
import os
import re
import tomllib
from pathlib import Path
from typing import Any

from .common import ROOT, fail, relative_name

TARGET_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*")
VALUE_NAME = re.compile(r"[A-Za-z0-9._-]+")
TARGET_SCHEMA = "fplinux.target/v1"
PLATFORM_SCHEMA = "fplinux.platform/v1"
RELEASE_SCHEMA = "fplinux.release/v1"


def exact_table(value: object, keys: set[str], name: str) -> dict[str, Any]:
    """Require an exact-key TOML table."""
    if not isinstance(value, dict) or set(value) != keys:
        fail(f"{name} must contain exactly: {', '.join(sorted(keys))}")
    return value


def nonempty_string(value: object, name: str) -> str:
    """Require a non-empty string."""
    if not isinstance(value, str) or not value:
        fail(f"{name} must be a non-empty string")
    return value


def relative_value(value: object, name: str) -> str:
    """Require a normalized relative path."""
    return relative_name(value, field=name)


def string_array(value: object, name: str, *, allow_empty: bool = False) -> list[str]:
    """Require an array of unique non-empty strings."""
    if not isinstance(value, list) or (not value and not allow_empty):
        qualifier = "an array" if allow_empty else "a non-empty array"
        fail(f"{name} must be {qualifier}")
    result = [nonempty_string(item, name) for item in value]
    if len(result) != len(set(result)):
        fail(f"{name} must not contain duplicates")
    return result


def path_array(value: object, name: str, *, allow_empty: bool = False) -> list[str]:
    """Require an array of normalized relative paths."""
    result = string_array(value, name, allow_empty=allow_empty)
    return [relative_value(item, name) for item in result]


def integer_value(
    value: object,
    name: str,
    *,
    bounds: tuple[int, int],
    alignment: int = 1,
) -> int:
    """Require a bounded, optionally aligned integer."""
    minimum, maximum = bounds
    if type(value) is not int or not minimum <= value <= maximum:
        fail(f"{name} must be an integer in {minimum}..{maximum}")
    if value % alignment:
        fail(f"{name} must be aligned to {alignment} bytes")
    return value


def path_steps(value: object, name: str) -> list[dict[str, str]]:
    """Validate typed source-to-destination projection steps."""
    if not isinstance(value, list):
        fail(f"{name} must be an array")
    result: list[dict[str, str]] = []
    for index, raw in enumerate(value):
        step = exact_table(raw, {"source", "destination"}, f"{name}[{index}]")
        result.append(
            {
                "source": relative_value(step.get("source"), f"{name}[{index}] source"),
                "destination": relative_value(
                    step.get("destination"),
                    f"{name}[{index}] destination",
                ),
            }
        )
    return result


def validate_usb(value: object, name: str) -> dict[str, Any]:
    """Validate USB identity and timeout metadata."""
    table = exact_table(value, {"vendor_id", "product_id", "wait_seconds"}, name)
    integer_value(table.get("vendor_id"), f"{name} vendor_id", bounds=(0, 0xFFFF))
    integer_value(table.get("product_id"), f"{name} product_id", bounds=(0, 0xFFFF))
    integer_value(table.get("wait_seconds"), f"{name} wait_seconds", bounds=(1, 3600))
    return table


def discover_targets() -> tuple[str, ...]:
    """Discover data-only target manifests without a central registry."""
    root = ROOT / "targets"
    targets = tuple(
        path.name
        for path in sorted(root.iterdir())
        if path.is_dir()
        and not path.is_symlink()
        and TARGET_NAME.fullmatch(path.name) is not None
        and (path / "target.toml").is_file()
        and not (path / "target.toml").is_symlink()
    )
    if not targets:
        fail("no targets are defined")
    return targets


def load_target(target: str) -> dict[str, Any]:
    """Load the exact declarative target v2 schema."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    path = ROOT / "targets" / target / "target.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown target: {target}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    config = exact_table(
        raw,
        {
            "schema",
            "name",
            "display_name",
            "release_slug",
            "platform",
            "profile",
            "release_manifest",
            "assets_lock",
            "buildroot",
            "linux",
            "bootstrap",
            "runtime",
        },
        f"target {target}",
    )
    if config.get("schema") != TARGET_SCHEMA:
        fail(f"target schema must be {TARGET_SCHEMA}: {path}")
    if config.get("name") != target:
        fail(f"target name does not match its directory: {path}")
    display_name = nonempty_string(config.get("display_name"), f"target {target} display_name")
    if not display_name.strip():
        fail(f"target {target} display_name must not be blank")
    for key in ("release_slug", "platform", "profile"):
        value = nonempty_string(config.get(key), f"target {target} {key}")
        if VALUE_NAME.fullmatch(value) is None:
            fail(f"target {target} has invalid {key}: {path}")
    for key in ("release_manifest", "assets_lock"):
        relative_value(config.get(key), f"target {target} {key}")

    buildroot = exact_table(config.get("buildroot"), {"defconfig"}, "target buildroot")
    relative_value(buildroot.get("defconfig"), "target buildroot defconfig")

    linux = exact_table(
        config.get("linux"),
        {
            "defconfig",
            "dtb",
            "debug_dtb",
            "patches",
            "copies",
            "appends",
            "forbidden_config",
        },
        "target linux",
    )
    relative_value(linux.get("defconfig"), "target linux defconfig")
    relative_value(linux.get("dtb"), "target linux dtb")
    relative_value(linux.get("debug_dtb"), "target linux debug_dtb")
    path_array(linux.get("patches"), "target linux patches", allow_empty=True)
    path_steps(linux.get("copies"), "target linux copies")
    path_steps(linux.get("appends"), "target linux appends")
    string_array(linux.get("forbidden_config"), "target linux forbidden_config")

    bootstrap = exact_table(
        config.get("bootstrap"),
        {
            "source",
            "image",
            "map",
            "kernel_destination",
            "dtb_destination",
            "load_address",
            "payload_limit",
            "toolchain",
            "lto",
        },
        "target bootstrap",
    )
    for key in ("source", "image", "map", "kernel_destination", "dtb_destination"):
        relative_value(bootstrap.get(key), f"target bootstrap {key}")
    integer_value(
        bootstrap.get("load_address"),
        "target bootstrap load_address",
        bounds=(0, 0xFFFFFFFF),
        alignment=4,
    )
    integer_value(
        bootstrap.get("payload_limit"),
        "target bootstrap payload_limit",
        bounds=(1, 0x100000000),
        alignment=4,
    )
    nonempty_string(bootstrap.get("toolchain"), "target bootstrap toolchain")
    integer_value(bootstrap.get("lto"), "target bootstrap lto", bounds=(0, 1))

    runtime = exact_table(
        config.get("runtime"),
        {"fdl1_load_address", "assets", "adapter", "usb"},
        "target runtime",
    )
    integer_value(
        runtime.get("fdl1_load_address"),
        "target runtime fdl1_load_address",
        bounds=(0, 0xFFFFFFFF),
        alignment=4,
    )
    assets = runtime.get("assets")
    if not isinstance(assets, dict) or not assets:
        fail("target runtime assets must be a non-empty table")
    for role, asset in assets.items():
        nonempty_string(role, "target runtime asset role")
        relative_value(asset, f"target runtime asset {role}")
    adapter = runtime.get("adapter")
    if not isinstance(adapter, dict) or not adapter:
        fail("target runtime adapter must be a non-empty table")
    usb = exact_table(runtime.get("usb"), {"bootrom", "linux_console"}, "target runtime usb")
    validate_usb(usb.get("bootrom"), "target runtime bootrom USB")
    validate_usb(usb.get("linux_console"), "target runtime linux_console USB")
    return config


def load_release(target: str, config: dict[str, Any]) -> dict[str, Any]:
    """Load the data-only release v2 schema for one target."""
    path = ROOT / "targets" / target / config["release_manifest"]
    if path.is_symlink() or not path.is_file():
        fail(f"target release manifest is missing or invalid: {path}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    manifest = exact_table(
        raw,
        {"schema", "image", "bundle_files", "documents"},
        f"target {target} release manifest",
    )
    if manifest.get("schema") != RELEASE_SCHEMA:
        fail(f"release manifest schema must be {RELEASE_SCHEMA}: {path}")
    image = relative_value(manifest.get("image"), "release manifest image")
    bundle_files = path_array(manifest.get("bundle_files"), "release bundle_files")
    documents = path_array(manifest.get("documents"), "release documents")
    if image not in bundle_files:
        fail("release manifest image must be a bundle file")
    return {
        "schema": RELEASE_SCHEMA,
        "image": image,
        "bundle_files": bundle_files,
        "documents": documents,
    }


def validate_host_tool(value: object, index: int) -> dict[str, Any]:
    """Validate one typed host build recipe."""
    name = f"platform host tools[{index}]"
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    recipe_type = value.get("type")
    if recipe_type == "make-archive/v1":
        recipe = exact_table(
            value,
            {
                "type",
                "name",
                "source_lock",
                "cache_name",
                "archive_prefix",
                "source_directory",
                "binary",
                "members",
            },
            name,
        )
        for key in ("name", "source_lock", "cache_name", "archive_prefix", "binary"):
            nonempty_string(recipe.get(key), f"{name} {key}")
        relative_value(recipe.get("source_directory"), f"{name} source_directory")
        members = recipe.get("members")
        if not isinstance(members, list) or not members:
            fail(f"{name} members must be a non-empty array")
        for member_index, raw_member in enumerate(members):
            member = exact_table(
                raw_member,
                {"path", "digest_key"},
                f"{name} members[{member_index}]",
            )
            relative_value(member.get("path"), f"{name} member path")
            nonempty_string(member.get("digest_key"), f"{name} member digest_key")
        return recipe
    if recipe_type == "cc-libusb/v1":
        recipe = exact_table(value, {"type", "name", "source", "self_test"}, name)
        nonempty_string(recipe.get("name"), f"{name} name")
        relative_value(recipe.get("source"), f"{name} source")
        if type(recipe.get("self_test")) is not bool:
            fail(f"{name} self_test must be a boolean")
        return recipe
    fail(f"{name} has unsupported type: {recipe_type}")
    return {}


def load_platform(platform: str) -> dict[str, Any]:
    """Load the exact reusable platform schema."""
    if TARGET_NAME.fullmatch(platform) is None:
        fail(f"invalid platform name: {platform}")
    path = ROOT / "platforms" / platform / "platform.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown platform: {platform}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    config = exact_table(
        raw,
        {"schema", "name", "buildroot", "linux", "bootstrap", "host", "runner"},
        f"platform {platform}",
    )
    if config.get("schema") != PLATFORM_SCHEMA:
        fail(f"platform schema must be {PLATFORM_SCHEMA}: {path}")
    if config.get("name") != platform:
        fail(f"platform name does not match its directory: {path}")

    buildroot = exact_table(
        config.get("buildroot"),
        {"external", "shared_paths", "clean_targets"},
        "platform buildroot",
    )
    relative_value(buildroot.get("external"), "platform buildroot external")
    path_array(buildroot.get("shared_paths"), "platform buildroot shared_paths", allow_empty=True)
    string_array(
        buildroot.get("clean_targets"),
        "platform buildroot clean_targets",
        allow_empty=True,
    )

    linux = exact_table(
        config.get("linux"),
        {
            "source_lock",
            "arch",
            "cross_compile",
            "analysis_cross_compile",
            "config_script",
            "image_output",
            "dtb_output_directory",
            "targets",
            "patches",
            "copies",
            "appends",
        },
        "platform linux",
    )
    for key in ("source_lock", "arch", "cross_compile", "analysis_cross_compile"):
        nonempty_string(linux.get(key), f"platform linux {key}")
    for key in ("config_script", "image_output", "dtb_output_directory"):
        relative_value(linux.get(key), f"platform linux {key}")
    string_array(linux.get("targets"), "platform linux targets")
    path_array(linux.get("patches"), "platform linux patches", allow_empty=True)
    path_steps(linux.get("copies"), "platform linux copies")
    path_steps(linux.get("appends"), "platform linux appends")

    bootstrap = exact_table(
        config.get("bootstrap"),
        {
            "vendor_source_lock",
            "vendor_cache_name",
            "archive_prefix",
            "source_destination",
            "vendor_destination",
            "output_destination",
            "pack_reloc",
            "safety_target",
            "build_targets",
            "files",
            "shared_copies",
        },
        "platform bootstrap",
    )
    for key in ("vendor_source_lock", "vendor_cache_name", "archive_prefix", "safety_target"):
        nonempty_string(bootstrap.get(key), f"platform bootstrap {key}")
    for key in ("source_destination", "vendor_destination", "output_destination", "pack_reloc"):
        relative_value(bootstrap.get(key), f"platform bootstrap {key}")
    string_array(bootstrap.get("build_targets"), "platform bootstrap build_targets")
    path_array(bootstrap.get("files"), "platform bootstrap files")
    path_steps(bootstrap.get("shared_copies"), "platform bootstrap shared_copies")

    host = exact_table(
        config.get("host"),
        {"capability", "runtime_tools", "tools"},
        "platform host",
    )
    nonempty_string(host.get("capability"), "platform host capability")
    tools = host.get("tools")
    if not isinstance(tools, list) or not tools:
        fail("platform host tools must be a non-empty array")
    validated_tools = [validate_host_tool(tool, index) for index, tool in enumerate(tools)]
    tool_names = [tool["name"] for tool in validated_tools]
    if len(tool_names) != len(set(tool_names)):
        fail("platform host tool names must be unique")
    runtime_tools = host.get("runtime_tools")
    if not isinstance(runtime_tools, dict) or not runtime_tools:
        fail("platform host runtime_tools must be a non-empty table")
    for role, tool_name in runtime_tools.items():
        nonempty_string(role, "platform host runtime role")
        if tool_name not in tool_names:
            fail(f"platform host runtime role {role} references an unknown tool")

    runner = exact_table(config.get("runner"), {"api"}, "platform runner")
    if runner.get("api") != "fplinux.host-adapter/v1":
        fail("platform runner api must be fplinux.host-adapter/v1")
    return config


def verified_runtime_digest(target: str) -> str | None:
    """Return the hardware-qualified runtime closure digest, if present."""
    path = ROOT / "releases.lock.toml"
    if not path.is_file():
        fail(f"release verification lock is missing: {path}")
    with path.open("rb") as stream:
        lock = tomllib.load(stream)
    digest = lock.get("verified", {}).get(target)
    if digest is None:
        return None
    if (
        not isinstance(digest, str)
        or len(digest) != 64
        or any(character not in "0123456789abcdef" for character in digest)
    ):
        fail(f"invalid verified runtime SHA256 for {target}: {path}")
    return digest


def validate_source_policy() -> None:
    """Enforce the single-image source policy."""
    ignored = {".cache", ".git", "out"}
    containerfiles: list[Path] = []
    # Prune the generated trees before descending: they hold millions of
    # cached build paths that a plain rglob would stat one by one.
    for directory, subdirectories, names in os.walk(ROOT):
        subdirectories[:] = [name for name in subdirectories if name not in ignored]
        for name in names:
            if name not in {"Containerfile", "Dockerfile"}:
                continue
            relative = (Path(directory) / name).relative_to(ROOT)
            if name == "Dockerfile":
                fail(f"unexpected additional container recipe: {relative}")
            containerfiles.append(relative)
    containerfiles.sort()
    if containerfiles != [Path("toolchains/Containerfile")]:
        fail("source must contain exactly toolchains/Containerfile")
    instructions = [
        line.strip()
        for line in (ROOT / containerfiles[0]).read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    from_instructions = [line for line in instructions if line.upper().startswith("FROM ")]
    if from_instructions != ["FROM ${BASE_IMAGE}"] or instructions[0] != "ARG BASE_IMAGE":
        fail("toolchains/Containerfile must use one lock-provided FROM ${BASE_IMAGE}")


def load_toolchain() -> dict[str, Any]:
    """Load the one pinned OCI/Buildroot environment lock."""
    validate_source_policy()
    path = ROOT / "toolchains/lock.toml"
    with path.open("rb") as stream:
        lock = tomllib.load(stream)
    if set(lock) != {"schema", "oci", "buildroot"} or lock.get("schema") != (
        "fplinux.toolchains/v1"
    ):
        fail(f"invalid toolchain lock schema: {path}")
    oci = lock.get("oci")
    if not isinstance(oci, dict) or set(oci) != {
        "image",
        "platform",
        "base",
        "base_created",
        "debian_snapshot",
    }:
        fail(f"toolchain lock must define exactly one OCI image: {path}")
    image = oci.get("image")
    base = oci.get("base")
    if not isinstance(image, str) or not image.startswith("localhost/"):
        fail(f"toolchain image must be one local tag: {path}")
    if not isinstance(base, str) or re.fullmatch(r"[^@\s]+@sha256:[0-9a-f]{64}", base) is None:
        fail(f"toolchain base image must be digest-pinned: {path}")
    if oci.get("platform") != "linux/amd64":
        fail(f"unsupported toolchain platform: {path}")
    buildroot = lock.get("buildroot")
    if not isinstance(buildroot, dict) or set(buildroot) != {
        "version",
        "url",
        "sha256",
        "bytes",
        "released",
    }:
        fail(f"invalid Buildroot lock entry: {path}")
    return lock


def toolchain_recipe_digest() -> str:
    """Hash every source input of the single OCI recipe."""
    fixed = [
        ROOT / ".containerignore",
        ROOT / "package.json",
        ROOT / "package-lock.json",
        ROOT / "scripts/fplinux_cli/common.py",
        ROOT / "scripts/fplinux_cli/config.py",
        ROOT / "scripts/fplinux_cli/container.py",
    ]
    for path in fixed:
        if path.is_symlink() or not path.is_file():
            fail(f"toolchain recipe input is missing or invalid: {path}")
    value = hashlib.sha256()
    paths = [*fixed, *sorted((ROOT / "toolchains").rglob("*"))]
    for path in paths:
        if path.is_symlink():
            fail(f"toolchain recipe must not contain symlinks: {path}")
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT).as_posix()
        value.update(relative.encode())
        value.update(b"\0")
        value.update(path.read_bytes())
        value.update((path.stat().st_mode & 0o777).to_bytes(2, "big"))
        value.update(b"\0")
    return value.hexdigest()

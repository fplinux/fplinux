# SPDX-License-Identifier: GPL-2.0-only
"""Load and validate targets, platforms and the build environment lock."""

from __future__ import annotations

import hashlib
import os
import re
import tomllib
from pathlib import Path
from typing import Any, Protocol

from .common import ROOT, fail, relative_name

TARGET_NAME = re.compile(r"[a-z0-9][a-z0-9._-]*")
VALUE_NAME = re.compile(r"[A-Za-z0-9._-]+")


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


def package_array(value: object, name: str) -> list[str]:
    """Require an array of unique, path-safe package identifiers."""
    result = string_array(value, name, allow_empty=True)
    for package in result:
        relative_value(package, name)
        if VALUE_NAME.fullmatch(package) is None:
            fail(f"{name} must contain only value-name package identifiers")
    return result


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


def validate_usb(value: object, name: str, *, interface_fields: bool = False) -> dict[str, Any]:
    """Validate USB identity and timeout metadata."""
    base_fields = {"vendor_id", "product_id", "wait_seconds"}
    fields = base_fields | ({"keyboard_interface"} if interface_fields else set())
    table = exact_table(value, fields, name)
    integer_value(table.get("vendor_id"), f"{name} vendor_id", bounds=(0, 0xFFFF))
    integer_value(table.get("product_id"), f"{name} product_id", bounds=(0, 0xFFFF))
    integer_value(table.get("wait_seconds"), f"{name} wait_seconds", bounds=(1, 3600))
    for field in ("keyboard_interface",):
        if field in table:
            integer_value(table[field], f"{name} {field}", bounds=(0, 255))
    return table


def _sha256(value: object, name: str) -> str:
    """Require one lowercase SHA-256 digest."""
    if (
        not isinstance(value, str)
        or len(value) != 64
        or any(character not in "0123456789abcdef" for character in value)
    ):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return value


def load_asset_lock(path: Path) -> list[dict[str, Any]]:
    """Load the current pinned asset outputs used to construct a RAM bundle."""
    if path.is_symlink() or not path.is_file():
        fail(f"asset lock is missing or invalid: {path}")
    try:
        with path.open("rb") as stream:
            document = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"asset lock is invalid: {error}")
    root = exact_table(document, {"source"}, "asset lock")
    sources = root.get("source")
    if not isinstance(sources, list) or not sources:
        fail("asset lock source must be a non-empty array")

    source_ids: set[str] = set()
    roles: set[str] = set()
    paths: set[str] = set()
    result: list[dict[str, Any]] = []
    for index, raw_source in enumerate(sources):
        name = f"asset source[{index}]"
        source = exact_table(
            raw_source,
            {"id", "kind", "url", "sha256", "cache_name", "license", "output"},
            name,
        )
        source_id = nonempty_string(source.get("id"), f"{name} id")
        if source_id in source_ids:
            fail(f"{name} id must be unique")
        source_ids.add(source_id)
        kind = source.get("kind")
        if kind not in {"file", "7z"}:
            fail(f"{name} kind must be file or 7z")
        url = nonempty_string(source.get("url"), f"{name} url")
        if not url.startswith("https://"):
            fail(f"{name} url must use HTTPS")
        _sha256(source.get("sha256"), f"{name} source")
        relative_value(source.get("cache_name"), f"{name} cache_name")
        nonempty_string(source.get("license"), f"{name} license")
        outputs = source.get("output")
        if not isinstance(outputs, list) or not outputs:
            fail(f"{name} output must be a non-empty array")
        normalized_outputs: list[dict[str, Any]] = []
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
            role = nonempty_string(output.get("role"), f"{output_name} role")
            if role in roles:
                fail(f"{output_name} role must be unique")
            roles.add(role)
            relative = relative_value(output.get("path"), f"{output_name} path")
            if relative in paths:
                fail(f"asset output path is duplicated: {relative}")
            paths.add(relative)
            _sha256(output.get("sha256"), f"{output_name} output")
            if kind == "7z":
                relative_value(output.get("member"), f"{output_name} member")
            normalized_outputs.append(output)
        result.append({**source, "output": normalized_outputs})
    return result


def asset_bundle_paths(path: Path) -> dict[str, str]:
    """Derive bundle paths solely from the selected asset-lock outputs."""
    return {
        str(output["role"]): f"assets/{output['path']}"
        for source in load_asset_lock(path)
        for output in source["output"]
    }


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


def target_directory(target: str) -> Path:
    """Return one validated target directory in the fixed repository layout."""
    if TARGET_NAME.fullmatch(target) is None:
        fail(f"invalid target name: {target}")
    path = ROOT / "targets" / target
    if path.is_symlink() or not path.is_dir():
        fail(f"unknown target: {target}")
    return path


def target_release_manifest_path(target: str) -> Path:
    """Return the fixed release-manifest path for one target."""
    return target_directory(target) / "release/manifest.toml"


def target_asset_lock_path(target: str) -> Path:
    """Return the fixed loader asset-lock path for one target."""
    return target_directory(target) / "loader/assets.lock.toml"


def target_defconfig_path(target: str) -> Path:
    """Return the fixed kernel defconfig path for one target."""
    return target_directory(target) / "kernel/defconfig"


def load_target(target: str) -> dict[str, Any]:
    """Load one exact target definition and materialize its platform contract."""
    path = target_directory(target) / "target.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown target: {target}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    config = exact_table(
        raw,
        {
            "name",
            "display_name",
            "release_slug",
            "platform",
            "bundle",
            "linux",
            "bootstrap",
            "adapter",
        },
        f"target {target}",
    )
    if config.get("name") != target:
        fail(f"target name does not match its directory: {path}")
    display_name = nonempty_string(config.get("display_name"), f"target {target} display_name")
    if not display_name.strip():
        fail(f"target {target} display_name must not be blank")
    for key in ("release_slug", "platform"):
        value = nonempty_string(config.get(key), f"target {target} {key}")
        if VALUE_NAME.fullmatch(value) is None:
            fail(f"target {target} has invalid {key}: {path}")
    bundle = exact_table(config.get("bundle"), {"packages"}, "target bundle")
    package_array(bundle.get("packages"), "target bundle packages")

    linux = exact_table(
        config.get("linux"),
        {
            "dtb",
            "debug_dtb",
            "patches",
            "copies",
            "appends",
            "forbidden_config",
            "forbidden_dtb_markers",
        },
        "target linux",
    )
    relative_value(linux.get("dtb"), "target linux dtb")
    relative_value(linux.get("debug_dtb"), "target linux debug_dtb")
    path_array(linux.get("patches"), "target linux patches", allow_empty=True)
    path_steps(linux.get("copies"), "target linux copies")
    path_steps(linux.get("appends"), "target linux appends")
    string_array(linux.get("forbidden_config"), "target linux forbidden_config")
    string_array(linux.get("forbidden_dtb_markers"), "target linux forbidden_dtb_markers")

    bootstrap = exact_table(
        config.get("bootstrap"),
        {
            "image",
            "map",
            "dtb_destination",
        },
        "target bootstrap",
    )
    for key in ("image", "map", "dtb_destination"):
        relative_value(bootstrap.get(key), f"target bootstrap {key}")

    adapter = exact_table(
        config.get("adapter"),
        {
            "spi_mode",
            "lcd_id",
            "exec_distance",
            "backlight_channels",
            "backlight_level",
            "session_name",
            "handoff_marker",
            "boot_instructions",
        },
        "target adapter",
    )
    integer_value(adapter.get("spi_mode"), "target adapter spi_mode", bounds=(0, 3))
    integer_value(adapter.get("lcd_id"), "target adapter lcd_id", bounds=(0, 0xFFFFFFFF))
    integer_value(
        adapter.get("exec_distance"),
        "target adapter exec_distance",
        bounds=(0, 0xFFFF),
    )
    nonempty_string(adapter.get("backlight_channels"), "target adapter backlight_channels")
    integer_value(
        adapter.get("backlight_level"),
        "target adapter backlight_level",
        bounds=(0, 0x3F),
    )
    for key in ("session_name", "handoff_marker", "boot_instructions"):
        nonempty_string(adapter.get(key), f"target adapter {key}")

    platform = load_platform(str(config["platform"]))
    platform_bootstrap = platform["bootstrap"]
    platform_runtime = platform["runtime"]
    config["bootstrap"] = {
        "source": "bootstrap",
        "image": bootstrap["image"],
        "map": bootstrap["map"],
        "kernel_destination": platform_bootstrap["kernel_destination"],
        "dtb_destination": bootstrap["dtb_destination"],
        "load_address": platform_bootstrap["load_address"],
        "payload_limit": platform_bootstrap["payload_limit"],
        "toolchain": platform_bootstrap["toolchain"],
        "lto": platform_bootstrap["lto"],
    }
    config["runtime"] = {
        "fdl1_load_address": platform_runtime["fdl1_load_address"],
        "assets": asset_bundle_paths(target_asset_lock_path(target)),
        "adapter": {**platform_runtime["adapter"], **adapter},
        "usb": platform_runtime["usb"],
    }
    return config


def load_release(target: str) -> dict[str, Any]:
    """Load the exact data-only release manifest for one target."""
    path = target_release_manifest_path(target)
    if path.is_symlink() or not path.is_file():
        fail(f"target release manifest is missing or invalid: {path}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    manifest = exact_table(
        raw,
        {"image", "bundle_files", "runtime_files", "documents"},
        f"target {target} release manifest",
    )
    image = relative_value(manifest.get("image"), "release manifest image")
    bundle_files = path_array(manifest.get("bundle_files"), "release bundle_files")
    runtime_files = path_array(manifest.get("runtime_files"), "release runtime_files")
    documents = path_array(manifest.get("documents"), "release documents")
    if image not in runtime_files:
        fail("release manifest image must be a runtime file")
    if not set(runtime_files).issubset(bundle_files):
        fail("release runtime files must be bundle files")
    return {
        "image": image,
        "bundle_files": bundle_files,
        "runtime_files": runtime_files,
        "documents": documents,
    }


def validate_host_tool(value: object, index: int) -> dict[str, Any]:
    """Validate one typed host build recipe."""
    name = f"platform host tools[{index}]"
    if not isinstance(value, dict):
        fail(f"{name} must be a table")
    recipe_type = value.get("type")
    if recipe_type == "make-archive":
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
                "link",
                "members",
            },
            name,
        )
        for key in ("name", "source_lock", "cache_name", "archive_prefix", "binary"):
            nonempty_string(recipe.get(key), f"{name} {key}")
        if recipe.get("link") != "static-libusb":
            fail(f"{name} link must be static-libusb")
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
    if recipe_type == "cc-libusb":
        recipe = exact_table(value, {"type", "name", "source", "self_test"}, name)
        nonempty_string(recipe.get("name"), f"{name} name")
        relative_value(recipe.get("source"), f"{name} source")
        if type(recipe.get("self_test")) is not bool:
            fail(f"{name} self_test must be a boolean")
        return recipe
    fail(f"{name} has unsupported type: {recipe_type}")
    return {}


def load_platform(platform: str) -> dict[str, Any]:
    """Load one exact reusable platform definition."""
    if TARGET_NAME.fullmatch(platform) is None:
        fail(f"invalid platform name: {platform}")
    path = ROOT / "platforms" / platform / "platform.toml"
    if path.is_symlink() or not path.is_file():
        fail(f"unknown platform: {platform}")
    with path.open("rb") as stream:
        raw = tomllib.load(stream)
    config = exact_table(
        raw,
        {"name", "rootfs", "bundle", "linux", "bootstrap", "runtime", "host"},
        f"platform {platform}",
    )
    if config.get("name") != platform:
        fail(f"platform name does not match its directory: {path}")

    rootfs = exact_table(config.get("rootfs"), {"packages"}, "platform rootfs")
    package_array(rootfs.get("packages"), "platform rootfs packages")
    bundle = exact_table(config.get("bundle"), {"packages"}, "platform bundle")
    package_array(bundle.get("packages"), "platform bundle packages")

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
            "kernel_destination",
            "load_address",
            "payload_limit",
            "toolchain",
            "lto",
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
    relative_value(bootstrap.get("kernel_destination"), "platform bootstrap kernel_destination")
    integer_value(
        bootstrap.get("load_address"),
        "platform bootstrap load_address",
        bounds=(0, 0xFFFFFFFF),
        alignment=4,
    )
    integer_value(
        bootstrap.get("payload_limit"),
        "platform bootstrap payload_limit",
        bounds=(1, 0x100000000),
        alignment=4,
    )
    nonempty_string(bootstrap.get("toolchain"), "platform bootstrap toolchain")
    integer_value(bootstrap.get("lto"), "platform bootstrap lto", bounds=(0, 1))

    runtime = exact_table(
        config.get("runtime"),
        {"fdl1_load_address", "adapter", "usb"},
        "platform runtime",
    )
    integer_value(
        runtime.get("fdl1_load_address"),
        "platform runtime fdl1_load_address",
        bounds=(0, 0xFFFFFFFF),
        alignment=4,
    )
    adapter = exact_table(
        runtime.get("adapter"),
        {
            "brightness",
            "rotation",
            "handoff_wait_seconds",
            "release_wait_seconds",
        },
        "platform runtime adapter",
    )
    integer_value(adapter.get("brightness"), "platform adapter brightness", bounds=(0, 100))
    integer_value(
        adapter.get("rotation"),
        "platform adapter rotation",
        bounds=(0, 270),
    )
    if adapter["rotation"] not in {0, 90, 180, 270}:
        fail("platform adapter rotation must be 0, 90, 180 or 270")
    integer_value(
        adapter.get("handoff_wait_seconds"),
        "platform adapter handoff_wait_seconds",
        bounds=(1, 3600),
    )
    integer_value(
        adapter.get("release_wait_seconds"),
        "platform adapter release_wait_seconds",
        bounds=(1, 300),
    )
    usb = exact_table(runtime.get("usb"), {"bootrom", "linux_gadget"}, "platform runtime usb")
    validate_usb(usb.get("bootrom"), "platform runtime bootrom USB")
    validate_usb(
        usb.get("linux_gadget"),
        "platform runtime linux_gadget USB",
        interface_fields=True,
    )

    host = exact_table(
        config.get("host"),
        {"runtime_tools", "tools"},
        "platform host",
    )
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
    ignored = {".cache", ".git"}
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
    if containerfiles != [Path("Containerfile")]:
        fail("source must contain exactly one root Containerfile")
    instructions = [
        line.strip()
        for line in (ROOT / containerfiles[0]).read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    from_instructions = [line for line in instructions if line.upper().startswith("FROM ")]
    if from_instructions != ["FROM ${BASE_IMAGE}"] or instructions[0] != "ARG BASE_IMAGE":
        fail("Containerfile must use one lock-provided FROM ${BASE_IMAGE}")


def load_container_lock() -> dict[str, Any]:
    """Load the one pinned OCI build-environment lock."""
    validate_source_policy()
    path = ROOT / "container.lock.toml"
    with path.open("rb") as stream:
        lock = tomllib.load(stream)
    if set(lock) != {"oci"}:
        fail(f"container lock must contain exactly oci: {path}")
    oci = lock.get("oci")
    if not isinstance(oci, dict) or set(oci) != {
        "repository",
        "platform",
        "base",
        "base_created",
    }:
        fail(f"container lock must define exactly one OCI repository: {path}")
    repository = oci.get("repository")
    base = oci.get("base")
    if repository != "localhost/fplinux-build":
        fail(f"container repository must be localhost/fplinux-build: {path}")
    if not isinstance(base, str) or re.fullmatch(r"[^@\s]+@sha256:[0-9a-f]{64}", base) is None:
        fail(f"container base image must be digest-pinned: {path}")
    if oci.get("platform") != "linux/amd64":
        fail(f"unsupported container platform: {path}")
    return lock


class _Hash(Protocol):
    """Describe the incremental hash operation used by recipe encoders."""

    def update(self, data: bytes) -> None:
        """Add bytes to the digest state."""


def _length_prefixed(value: _Hash, data: bytes) -> None:
    """Add one unambiguous byte field to an incremental recipe digest."""
    value.update(len(data).to_bytes(8, "big"))
    value.update(data)


def _stable_regular_file(path: Path) -> tuple[bytes, int]:
    """Read one regular causal recipe input."""
    if path.is_symlink() or not path.is_file():
        fail(f"recipe input is missing or invalid: {path}")
    try:
        return path.read_bytes(), path.stat().st_mode & 0o777
    except OSError as error:
        fail(f"recipe input cannot be read: {path}: {error}")


def _exact_file_recipe(paths: list[Path], *, prefix: bytes = b"") -> str:
    """Hash exact logical paths, bytes and modes for one causal file closure."""
    value = hashlib.sha256()
    _length_prefixed(value, prefix)
    for path in paths:
        contents, mode = _stable_regular_file(path)
        relative = path.relative_to(ROOT).as_posix()
        _length_prefixed(value, relative.encode())
        _length_prefixed(value, contents)
        _length_prefixed(value, mode.to_bytes(2, "big"))
    return value.hexdigest()


def container_image_build_arguments(lock: dict[str, Any] | None = None) -> tuple[str, ...]:
    """Return exact causal Podman build arguments, excluding tag and recipe label."""
    if lock is None:
        lock = load_container_lock()
    oci = lock["oci"]
    return (
        "--platform",
        oci["platform"],
        "--file",
        "Containerfile",
        "--build-arg",
        f"BASE_IMAGE={oci['base']}",
    )


def container_image_recipe_digest(lock: dict[str, Any] | None = None) -> str:
    """Hash only exact context bytes, modes, paths and build arguments."""
    arguments = (*container_image_build_arguments(lock), ".")
    encoded_arguments = b"".join(
        len(argument.encode()).to_bytes(8, "big") + argument.encode() for argument in arguments
    )
    return _exact_file_recipe(
        [
            ROOT / ".containerignore",
            ROOT / "Containerfile",
            ROOT / "package.json",
            ROOT / "package-lock.json",
        ],
        prefix=encoded_arguments,
    )


def container_image_reference(
    lock: dict[str, Any] | None = None, recipe: str | None = None
) -> str:
    """Return the recipe-addressed local reference for one build image."""
    if lock is None:
        lock = load_container_lock()
    if recipe is None:
        recipe = container_image_recipe_digest(lock)
    if len(recipe) != 64 or any(character not in "0123456789abcdef" for character in recipe):
        fail("container image recipe must be a lowercase SHA-256")
    return f"{lock['oci']['repository']}:{recipe}"


def check_orchestration_recipe_digest(image_recipe: str | None = None) -> str:
    """Hash only the implementation that can change cached check results."""
    fixed = [
        ROOT / "scripts/fplinux_cli/checkreceipts.py",
        ROOT / "scripts/fplinux_cli/common.py",
        ROOT / "scripts/fplinux_cli/config.py",
        ROOT / "scripts/fplinux_cli/container.py",
        ROOT / "scripts/fplinux_cli/image_state.py",
        ROOT / "scripts/fplinux_cli/output.py",
        ROOT / "scripts/fplinux_cli/workspace.py",
    ]
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    if len(image_recipe) != 64 or any(
        character not in "0123456789abcdef" for character in image_recipe
    ):
        fail("container image recipe must be a lowercase SHA-256")
    return _exact_file_recipe(fixed, prefix=bytes.fromhex(image_recipe))

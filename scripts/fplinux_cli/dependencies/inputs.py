# SPDX-License-Identifier: GPL-2.0-only
"""Describe exact external inputs and the declarations that select them."""

from __future__ import annotations

import base64
import binascii
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any
from urllib.parse import urlsplit

from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.alpine import selection as alpine_selection
from fplinux_cli.alpine.sources import aport_declaration, aport_sources
from fplinux_cli.build.host_tools.phone_data import preparation_sources
from fplinux_cli.build.runtime import (
    runtime_host_tools,
    runtime_needs_nand_ecc,
    selected_host_tool_recipes,
)
from fplinux_cli.common import fail, load_toml, relative_name, sha256_bytes
from fplinux_cli.environment.definition import (
    load_environment_lock,
    spreadtrum_flash_environment_source,
)
from fplinux_cli.environment.downloads import download_locked_file
from fplinux_cli.manifests.assets import load_asset_sources
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.loaders import loader_sources
from fplinux_cli.manifests.paths import declared_boot_profiles
from fplinux_cli.manifests.source_catalog import ResolvedSource, load_host_sources, resolve_source
from fplinux_cli.manifests.values import sha256_value

if TYPE_CHECKING:
    from collections.abc import Sequence


@dataclass(frozen=True)
class DependencyInput:
    """One exact download, with the checksum its producer actually declares."""

    key: str
    url: str
    sha256: str | None
    size: int | None
    destination: str
    purpose: str
    arch: str | None = None
    checksum: str | None = None
    algorithm: str = "sha256"

    def __post_init__(self) -> None:
        """Reject records that cannot identify and restore exact bytes."""
        if not self.key or not self.purpose:
            fail("dependency input must have a key and purpose")
        parsed = urlsplit(self.url)
        if parsed.scheme != "https" or not parsed.netloc:
            fail(f"dependency {self.key} must use HTTPS")
        relative_name(self.destination, field=f"dependency {self.key} destination")
        if self.destination == ".":
            fail(f"dependency {self.key} destination must name a file")
        if self.size is not None and (type(self.size) is not int or self.size <= 0):
            fail(f"dependency {self.key} size must be a positive integer")
        if self.sha256 is not None:
            sha256_value(self.sha256, f"dependency {self.key}")
        expected = self.checksum if self.checksum is not None else self.sha256
        length = {"sha256": 64, "sha512": 128}.get(self.algorithm)
        if length is None or not isinstance(expected, str):
            fail(f"dependency {self.key} must have an exact SHA-256 or SHA-512 checksum")
        if len(expected) != length or re.fullmatch(r"[0-9a-f]+", expected) is None:
            fail(f"dependency {self.key} has an invalid {self.algorithm} checksum")
        if self.algorithm == "sha256" and self.sha256 is not None and expected != self.sha256:
            fail(f"dependency {self.key} has conflicting SHA-256 checksums")


def npm_declarations(
    path: Path, *, key_prefix: str, cache_directory: str, purpose: str
) -> tuple[dict[str, object], list[DependencyInput]]:
    """Read exact registry artifacts, including optional platform packages."""
    lock = json.loads(path.read_text(encoding="utf-8"))
    packages = lock.get("packages")
    if not isinstance(packages, dict):
        fail("package-lock.json must declare package records")
    inputs = []
    context = {}
    downloaded_packages: set[str] = set()
    for name, record in sorted(packages.items()):
        if name == "":
            context[name] = record
            continue
        context[name] = {
            field: record[field]
            for field in (
                "version",
                "resolved",
                "integrity",
                "inBundle",
                "bundleDependencies",
                "dependencies",
                "optionalDependencies",
                "peerDependencies",
                "peerDependenciesMeta",
                "optional",
                "os",
                "cpu",
                "libc",
            )
            if field in record
        }
        if record.get("inBundle") is True:
            owner = name
            while "/node_modules/" in owner:
                owner = owner.rsplit("/node_modules/", 1)[0]
                if owner in downloaded_packages:
                    break
            else:
                fail(f"bundled npm package has no exact parent registry input: {name}")
            continue
        url = record.get("resolved")
        integrity = record.get("integrity")
        if not isinstance(url, str) or not isinstance(integrity, str):
            fail(f"npm package has no exact registry input: {name}")
        algorithm, separator, encoded = integrity.partition("-")
        if not separator or algorithm not in {"sha256", "sha512"}:
            fail(f"npm package has an unsupported integrity checksum: {name}")
        try:
            checksum = base64.b64decode(encoded, validate=True).hex()
        except binascii.Error:
            fail(f"npm package has an invalid integrity checksum: {name}")
        filename = PurePosixPath(urlsplit(url).path).name
        url_key = sha256_bytes(url.encode())[:16]
        inputs.append(
            DependencyInput(
                key=f"{key_prefix}:{name}",
                url=url,
                sha256=checksum if algorithm == "sha256" else None,
                size=None,
                destination=f"{cache_directory}/{url_key}-{filename}",
                purpose=purpose,
                checksum=checksum,
                algorithm=algorithm,
            )
        )
        downloaded_packages.add(name)
    return {"packages": context}, inputs


def _selected_declarations(
    root: Path,
) -> tuple[dict[str, Any], dict[str, Any], set[str], dict[str, Any]]:
    """Select the package producers reachable through current targets and profiles."""
    targets = {}
    raw_targets = {}
    platforms = {}
    packages: set[str] = set()
    for path in sorted((root / "targets").glob("*/target.toml")):
        target = load_toml(path)
        raw_targets[path.parent.name] = target
        platform_name = target["platform"]
        if platform_name not in platforms:
            platforms[platform_name] = load_toml(
                root / "platforms" / platform_name / "platform.toml"
            )
        platform = platforms[platform_name]
        selected_profiles = declared_boot_profiles(target)
        profile_context = {}
        for profile in selected_profiles:
            declaration = load_toml(root / "profiles" / profile / "profile.toml")
            for build_type in BUILD_TYPES:
                selected = {
                    **target,
                    "build_type": build_type,
                    "rootfs": {
                        "base_packages": target["rootfs"]["packages"],
                        "packages": declaration["rootfs"]["packages"],
                    },
                }
                rootfs_packages = alpine_selection.selected_packages(platform, selected, root)
                packages.update(rootfs_packages)
                packages.update(
                    alpine_selection.bundle_packages(platform, selected, rootfs_packages, root)
                )
            profile_context[profile] = {
                "packages": sorted(declaration["rootfs"]["packages"]),
                "bootstrap": declaration["bootstrap"]["kind"],
                "uboot": declaration["uboot"]["kind"],
            }
        targets[path.parent.name] = {
            "platform": platform_name,
            "rootfs": sorted(target["rootfs"]["packages"]),
            "bundle": sorted(target["bundle"]["packages"]),
            "profiles": profile_context,
        }
    if not targets:
        fail("dependency inventory requires at least one declared target")
    return targets, platforms, packages, raw_targets


def source_input(source: ResolvedSource) -> DependencyInput:
    """Describe one canonical direct original independently of its consumers."""
    return DependencyInput(
        key=f"source:{source.id}",
        url=source.url,
        sha256=source.checksum if source.algorithm == "sha256" else None,
        size=source.size,
        destination=source.destination,
        purpose="source-original",
        checksum=source.checksum if source.algorithm != "sha256" else None,
        algorithm=source.algorithm,
    )


def aport_source_inputs(root: Path, packages: Sequence[str]) -> list[DependencyInput]:
    """Resolve every upstream original in the selected native package closure."""
    selected = {}
    for package in alpine_selection.aport_build_order(packages):
        for source in aport_sources(root, package):
            selected[source.id] = source_input(source)
    return [selected[key] for key in sorted(selected)]


def acquire_inputs(
    inputs: Sequence[DependencyInput],
    cache: Path,
    *,
    offline: bool,
    setup_command: str = "./fplinux setup --all",
) -> dict[str, Path]:
    """Acquire selected originals once before their consumers use only local files."""
    result = {}
    for item in inputs:
        expected = item.checksum or item.sha256
        if expected is None:
            fail(f"dependency input has no checksum: {item.key}")
        result[item.key] = download_locked_file(
            item.url,
            expected,
            cache / item.destination,
            offline=offline,
            algorithm=item.algorithm,
            size=item.size,
            setup_command=setup_command,
        )
    return result


def alpine_package_input(lock: dict[str, Any], record: dict[str, Any]) -> DependencyInput:
    """Resolve one native binary APK for both full builds and selected sysroots."""
    return DependencyInput(
        key=f"alpine:{lock['arch']}:{record['file']}",
        url=f"{lock['repositories'][record['repository']]}/{record['file']}",
        sha256=record["sha256"],
        size=record["bytes"],
        destination=f"downloads/alpine/packages/{record['file']}",
        purpose="target-package",
        arch=lock["arch"],
    )


def _target_package_inputs(lock: dict[str, Any]) -> list[DependencyInput]:
    """Describe the phone's exact native binary APK closure at its established cache paths."""
    return [alpine_package_input(lock, record) for record in lock["package"]]


def captured_build_inputs(root: Path, source_declarations: bytes) -> list[DependencyInput]:
    """Resolve the captured source selection and native APKs from the same staged checkout."""
    declarations = json.loads(source_declarations)
    selected = {
        item.key: item
        for source_id in declarations["sources"]["records"]
        for item in (source_input(resolve_source(root, source_id)),)
    }
    for item in _target_package_inputs(alpine_lock.load_alpine_lock(root)):
        selected[item.key] = item
    return [selected[key] for key in sorted(selected)]


def build_source_inputs(
    root: Path, platform: dict[str, Any], config: dict[str, Any]
) -> list[DependencyInput]:
    """Resolve the exact package, platform and boot originals used by one build."""
    selected: dict[str, DependencyInput] = {}
    rootfs = alpine_selection.selected_packages(platform, config, root)
    packages = (*rootfs, *alpine_selection.bundle_packages(platform, config, rootfs, root))
    for item in aport_source_inputs(root, packages):
        selected[item.key] = item
    source_ids = {platform["linux"]["source_lock"]}
    if config.get("emmc") is not None:
        source_ids.update(source.id for source in preparation_sources(root))
    bootstrap = platform["bootstrap"]
    if bootstrap["backend"] == "ums9117":
        source_ids.add(bootstrap["vendor_source_lock"])
    host_tools = runtime_host_tools(platform, nand_ecc=runtime_needs_nand_ecc(config))
    source_ids.update(
        tool["source_lock"]
        for tool in selected_host_tool_recipes(platform, host_tools.values())
        if tool["type"] == "make-archive"
    )
    if config["uboot"]["kind"] == "full":
        source_ids.add(load_toml(root / platform["uboot"]["source"])["source"])
    if bootstrap["backend"] == "sc9820e":
        source_ids.update(
            source.source_lock
            for source in loader_sources(
                config["loader"]["build"], load_toml(root / "sources.lock.toml")
            )
        )
    else:
        source_ids.update(source["source_id"] for source in config["loader_assets"])
    for source_id in sorted(source_ids):
        item = source_input(resolve_source(root, source_id))
        selected[item.key] = item
    alpine = alpine_lock.load_alpine_lock(root)
    target_base = source_input(
        resolve_source(root, load_toml(root / "alpine.lock.toml")["minirootfs"]["source"])
    )
    selected[target_base.key] = target_base
    for item in _target_package_inputs(alpine):
        selected[item.key] = item
    return [selected[key] for key in sorted(selected)]


def _environment_lock(root: Path) -> tuple[dict[str, Any], list[DependencyInput]]:
    """Load the exact package closure selected for the build environment."""
    lock = load_environment_lock(root)
    records = lock["input"]
    direct = {
        record["key"]: record["source"]
        for record in load_toml(root / "environment.lock.toml")["input"]
        if "source" in record
    }
    inputs = [
        source_input(resolve_source(root, direct[record["key"]]))
        if record["key"] in direct
        else DependencyInput(
            key=record["key"],
            url=record["url"],
            sha256=record.get("sha256"),
            size=record.get("bytes"),
            destination=record["destination"],
            purpose=record["purpose"],
            arch=record.get("arch"),
            checksum=record.get("checksum"),
            algorithm=record.get("algorithm", "sha256"),
        )
        for record in records
    ]
    normalized = {
        **lock,
        "input": sorted(records, key=lambda record: record["key"]),
    }
    return normalized, inputs


def _environment_declarations(root: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    lock_context, inputs = _environment_lock(root)
    ui_fonts = ui_font_environment_inputs(root)
    inputs.extend(ui_fonts)
    terminal_fonts = terminus_font_environment_inputs(root)
    inputs.extend(terminal_fonts)
    ui_icons = ui_icon_environment_inputs(root)
    inputs.extend(ui_icons)
    quality_source = spreadtrum_flash_environment_source(root)
    inputs.append(source_input(resolve_source(root, "spreadtrum_flash")))
    npm_context, npm_inputs = npm_declarations(
        root / "package-lock.json",
        key_prefix="npm",
        cache_directory="downloads/npm",
        purpose="npm-package",
    )
    inputs.extend(npm_inputs)
    return {
        "npm": npm_context,
        "environment_lock": lock_context,
        "quality_sources": {
            "spreadtrum_flash": quality_source,
            "ui_fonts": [asdict(item) for item in ui_fonts],
            "terminal_fonts": [asdict(item) for item in terminal_fonts],
            "ui_icons": [asdict(item) for item in ui_icons],
        },
    }, inputs


def ui_font_environment_inputs(root: Path) -> list[DependencyInput]:
    """Select the UI package's pinned font and license downloads for offline rendering."""
    return [source_input(source) for source in aport_sources(root, "fplinux-ui")]


def terminus_font_environment_inputs(root: Path) -> list[DependencyInput]:
    """Select the bitmap font package's pinned archive for offline rendering."""
    return [source_input(source) for source in aport_sources(root, "fplinux-font-terminus")]


def ui_icon_environment_inputs(root: Path) -> list[DependencyInput]:
    """Select the icon package's pinned originals for offline native rendering."""
    return [source_input(source) for source in aport_sources(root, "fplinux-icons")]


def environment_inputs(root: Path) -> list[DependencyInput]:
    """Return the inputs consumed by environment setup without loading target data."""
    return sorted(_environment_declarations(root)[1], key=lambda item: item.key)


def _host_declarations(
    root: Path,
    hosts: Sequence[str] = ("linux-host", "windows-host", "windows-driver"),
) -> tuple[dict[str, object], list[DependencyInput]]:
    """Retain the originals consumed by autonomous runtime and driver preparation."""
    locks = {host: load_host_sources(root, f"{host}.lock.toml") for host in hosts}
    inputs = {
        entry["source_id"]: source_input(resolve_source(root, entry["source_id"]))
        for lock in locks.values()
        for entry in lock.values()
    }
    return dict(locks), list(inputs.values())


def host_inputs(root: Path, host: str) -> list[DependencyInput]:
    """Select originals for one standalone runtime and its native preparation tools."""
    if host == "linux":
        hosts: tuple[str, ...] = ("linux-host",)
    elif host == "windows":
        hosts = ("windows-host", "windows-driver")
    else:
        fail(f"unsupported runtime host: {host}")
    return deduplicate_inputs(_host_declarations(root, hosts)[1])


def _inventory(root: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    alpine = alpine_lock.load_alpine_lock(root)
    sources = load_toml(root / "sources.lock.toml")
    targets, platforms, packages, raw_targets = _selected_declarations(root)
    environment_context, inputs = _environment_declarations(root)
    host_dependencies, host_inputs = _host_declarations(root)
    inputs.extend(host_inputs)
    inputs.append(
        source_input(
            resolve_source(root, load_toml(root / "alpine.lock.toml")["minirootfs"]["source"])
        )
    )
    inputs.extend(_target_package_inputs(alpine))
    platform_context = {}
    active_sources = {}
    for name, platform in sorted(platforms.items()):
        linux = platform["linux"]
        source_name = linux["source_lock"]
        source = sources[source_name]
        inputs.append(source_input(resolve_source(root, source_name)))
        active_sources[source_name] = source
        bootstrap = platform["bootstrap"]
        if bootstrap["backend"] == "ums9117":
            source_name = bootstrap["vendor_source_lock"]
            inputs.append(source_input(resolve_source(root, source_name)))
            active_sources[source_name] = sources[source_name]
        host_context = []
        for tool in platform["host"]["tools"]:
            if tool["type"] != "make-archive":
                continue
            source_name = tool["source_lock"]
            inputs.append(source_input(resolve_source(root, source_name)))
            active_sources[source_name] = sources[source_name]
            host_context.append(
                {
                    field: tool[field]
                    for field in ("name", "source_lock", "archive_prefix", "members")
                }
            )
        platform_context[name] = {
            "rootfs": sorted(platform["rootfs"]["packages"]),
            "bundle": sorted(platform["bundle"]["packages"]),
            "linux_source": linux["source_lock"],
            "bootstrap_backend": bootstrap["backend"],
            "host": host_context,
        }
        if bootstrap["backend"] == "ums9117":
            platform_context[name]["bootstrap_source"] = bootstrap["vendor_source_lock"]
        if any(
            selection["profiles"].get("microsd-uboot", {}).get("uboot") == "full"
            for selection in targets.values()
            if selection["platform"] == name
        ):
            lock = load_toml(root / platform["uboot"]["source"])
            inputs.append(source_input(resolve_source(root, lock["source"])))
            platform_context[name]["uboot"] = lock
    asset_context = {}
    for target, selection in targets.items():
        if raw_targets[target].get("emmc") is not None:
            for source in preparation_sources(root):
                active_sources[source.id] = sources[source.id]
                inputs.append(source_input(source))
        if platforms[selection["platform"]]["bootstrap"]["backend"] == "sc9820e":
            selected_loaders = loader_sources(raw_targets[target]["loader"]["build"], sources)
            asset_context[target] = [asdict(source) for source in selected_loaders]
            for source in selected_loaders:
                active_sources[source.source_lock] = sources[source.source_lock]
                inputs.append(source_input(resolve_source(root, source.source_lock)))
            continue
        assets = load_asset_sources(
            root / "platforms" / selection["platform"] / "loader/assets.lock.toml",
            source_root=root,
        )
        asset_context[target] = assets
        for asset in assets:
            inputs.append(source_input(resolve_source(root, asset["source_id"])))
    aport_context = {}
    producers = sorted(alpine_selection.aport_build_order(sorted(packages)))
    for producer in producers:
        aport_context[producer] = aport_declaration(root, producer)
        inputs.extend(source_input(source) for source in aport_sources(root, producer))
    context: dict[str, object] = {
        **environment_context,
        "alpine": {
            **alpine,
            "runtime": {
                "packages": sorted(alpine["runtime"]["packages"]),
                "additions": {
                    name: sorted(values) for name, values in alpine["runtime"]["additions"].items()
                },
            },
            "sysroot": {"packages": sorted(alpine["sysroot"]["packages"])},
            "package": sorted(alpine["package"], key=lambda record: record["file"]),
        },
        "sources": active_sources,
        "hosts": host_dependencies,
        "targets": targets,
        "platforms": platform_context,
        "assets": asset_context,
        "producers": producers,
        "aports": aport_context,
    }
    return context, deduplicate_inputs(inputs)


def deduplicate_inputs(inputs: Sequence[DependencyInput]) -> list[DependencyInput]:
    """Merge shared consumers while rejecting conflicting originals or cache destinations."""
    unique: dict[str, DependencyInput] = {}
    destinations: dict[str, tuple[str | None, str | None, str]] = {}
    for item in inputs:
        previous = unique.get(item.key)
        if previous is not None and previous != item:
            fail(f"dependency key has conflicting declarations: {item.key}")
        identity = (item.sha256, item.checksum, item.algorithm)
        existing = destinations.get(item.destination)
        if existing is not None and existing != identity:
            fail(f"dependency cache destination has conflicting checksums: {item.destination}")
        unique[item.key] = item
        destinations[item.destination] = identity
    return [unique[key] for key in sorted(unique)]


def dependency_selection(root: Path) -> tuple[list[DependencyInput], dict[str, object]]:
    """Return exact downloads and their semantic selection from one declaration inventory."""
    context, inputs = _inventory(root)
    return inputs, {**context, "inputs": [asdict(item) for item in inputs]}

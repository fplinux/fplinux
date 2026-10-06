# SPDX-License-Identifier: GPL-2.0-only
"""Describe exact external inputs and the declarations that select them."""

from __future__ import annotations

import base64
import binascii
import fnmatch
import json
import re
import shlex
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any
from urllib.parse import urlsplit

from fplinux_cli.alpine import lock as alpine_lock
from fplinux_cli.alpine import registration as alpine_registration
from fplinux_cli.alpine import selection as alpine_selection
from fplinux_cli.common import fail, load_toml, relative_name, sha256_bytes
from fplinux_cli.manifests.assets import load_asset_lock
from fplinux_cli.manifests.values import sha256_value

if TYPE_CHECKING:
    from collections.abc import Iterable

_ASSIGNMENT = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)=(\"(?:[^\"\\]|\\.)*\"|'[^']*'|[^\n]*)",
    re.MULTILINE,
)
_VARIABLE = re.compile(r"\$\{([^{}]+)\}|\$([A-Za-z_][A-Za-z0-9_]*)")
_SOURCE_LOOP = re.compile(
    r"^for ([A-Za-z_][A-Za-z0-9_]*) in ([^;]+); do\s*"
    r'source="\$source ([^"\n]+)"\s*done$',
    re.MULTILINE,
)


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


def _expand(value: str, variables: dict[str, str], *, owner: str) -> str:
    """Expand the literal parameter substitutions used in source declarations."""
    if "$(" in value or "`" in value:
        fail(f"{owner} dependency declaration uses an unsupported command substitution")

    def replace(match: re.Match[str]) -> str:
        expression = match[1] or match[2]
        parsed = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)(.*)", expression)
        if parsed is None:
            fail(f"{owner} dependency declaration has an unsupported variable: {expression}")
        name, operation = parsed.groups()
        if name not in variables:
            fail(f"{owner} dependency declaration has an unknown variable: {name}")
        expanded = _expand(
            variables[name], {k: v for k, v in variables.items() if k != name}, owner=owner
        )
        if not operation:
            return expanded
        if operation.startswith("/"):
            parts = operation.split("/")
            if len(parts) == 3 and parts[1]:
                return expanded.replace(parts[1], parts[2], 1)
        for operator in ("%%", "%", "##", "#"):
            if not operation.startswith(operator):
                continue
            pattern = operation.removeprefix(operator)
            positions = range(len(expanded) + 1)
            indices: Iterable[int] = positions
            if operator in {"%", "##"}:
                indices = reversed(positions)
            for index in indices:
                candidate = expanded[index:] if operator.startswith("%") else expanded[:index]
                if fnmatch.fnmatchcase(candidate, pattern):
                    return expanded[:index] if operator.startswith("%") else expanded[index:]
            return expanded
        return fail(
            f"{owner} dependency declaration has an unsupported substitution: {expression}"
        )

    result = _VARIABLE.sub(replace, value)
    if "$" in result:
        fail(f"{owner} dependency declaration contains unresolved shell syntax")
    return result


def _apkbuild_declarations(path: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    """Read maintained source declarations without executing the package recipe."""
    text = path.read_text(encoding="utf-8").replace("\\\n", "")
    raw = {match[1]: match[2] for match in _ASSIGNMENT.finditer(text)}
    variables: dict[str, str] = {}
    for name, value in raw.items():
        if value.startswith(("'", '"')):
            variables[name] = value[1:-1]
        else:
            variables[name] = value.split(" #", 1)[0].rstrip()
    owner = path.parent.name
    sources = _expand(variables.get("source", ""), variables, owner=owner).split()
    source_loops = list(_SOURCE_LOOP.finditer(text))
    remaining = _SOURCE_LOOP.sub("", text)
    source_assignments = [
        match for match in _ASSIGNMENT.finditer(remaining) if match[1] == "source"
    ]
    without_sources = remaining
    for assignment in reversed(source_assignments):
        without_sources = (
            without_sources[: assignment.start()] + without_sources[assignment.end() :]
        )
    if re.search(r"^\s*source=", without_sources, re.MULTILINE):
        fail(f"{owner} has an unsupported conditional source declaration")
    if re.search(r"^for .*\n(?:(?!^done).)*\bsource=", remaining, re.MULTILINE | re.DOTALL):
        fail(f"{owner} has an unsupported source declaration loop")
    for loop in source_loops:
        name, values, template = loop.groups()
        for value in shlex.split(values):
            sources.extend(_expand(template, {**variables, name: value}, owner=owner).split())
    checksum_words = _expand(variables.get("sha512sums", ""), variables, owner=owner).split()
    if len(checksum_words) % 2:
        fail(f"{owner} has an invalid sha512sums declaration")
    checksums = dict(zip(checksum_words[1::2], checksum_words[::2], strict=True))
    arch = _expand(variables.get("arch", ""), variables, owner=owner)
    inputs: list[DependencyInput] = []
    source_context: list[dict[str, str]] = []
    for source in sources:
        if "::" in source:
            filename, url = source.split("::", 1)
        else:
            url = source
            filename = PurePosixPath(urlsplit(url).path).name
        if not url.startswith("https://"):
            if "://" in url:
                fail(f"{owner} source must use HTTPS: {url}")
            source_context.append({"file": source})
            continue
        checksum = checksums.get(filename)
        if checksum is None:
            fail(f"{owner} external source has no checksum: {filename}")
        inputs.append(
            DependencyInput(
                key=f"aport:{owner}:{filename}",
                url=url,
                sha256=None,
                size=None,
                destination=f"downloads/alpine/sources/{filename}",
                purpose="aport-source",
                arch=arch or None,
                checksum=checksum,
                algorithm="sha512",
            )
        )
        source_context.append({"file": filename, "url": url, "sha512": checksum})
    context: dict[str, object] = {"sources": source_context}
    for name in (
        "pkgname",
        "pkgver",
        "pkgrel",
        "arch",
        "provides",
        "provider_priority",
        "replaces",
        "depends",
        "makedepends_build",
        "makedepends_host",
        "subpackages",
    ):
        value = _expand(variables.get(name, ""), variables, owner=owner)
        context[name] = (
            sorted(value.split()) if name not in {"pkgname", "pkgver", "pkgrel"} else value
        )
    return context, inputs


def _container_declarations(root: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    """Read pinned curl downloads and ordered package-install groups."""
    text = (root / "Containerfile").read_text(encoding="utf-8").replace("\\\n", "")
    text = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    arguments = dict(re.findall(r"^ARG ([A-Za-z_][A-Za-z0-9_]*)=(\S+)\s*$", text, re.MULTILINE))
    package_groups = [
        sorted(shlex.split(match[1]))
        for match in re.finditer(r"\bapk add --no-cache\s+([^;\n]+)", text)
    ]
    inputs = []
    downloads = []
    pattern = re.compile(
        r'\bcurl\s+[^;]*?["\'](https://[^"\']+)["\']\s*;\s*(?:fi;\s*)?'
        r'printf [^;]*?["\']\$\{([A-Za-z_][A-Za-z0-9_]*)\}["\']'
        r"[^;]*?\| sha(256|512)sum -c -",
    )
    for match in pattern.finditer(text):
        url = _expand(match[1], arguments, owner="Containerfile")
        checksum_name = match[2]
        checksum = arguments.get(checksum_name)
        if checksum is None:
            fail(f"Containerfile download has no pinned checksum: {url}")
        algorithm = f"sha{match[3]}"
        name = checksum_name.removesuffix(f"_SHA{match[3]}").lower().replace("_", "-")
        filename = PurePosixPath(urlsplit(url).path).name
        inputs.append(
            DependencyInput(
                key=f"container:{name}",
                url=url,
                sha256=checksum if algorithm == "sha256" else None,
                size=None,
                destination=f"downloads/environment/{name}/{filename}",
                purpose="container-source",
                arch="x86_64",
                checksum=checksum,
                algorithm=algorithm,
            )
        )
        downloads.append({"url": url, "algorithm": algorithm, "checksum": checksum})
    declared_curls = len(re.findall(r"\bcurl\s+", text))
    if declared_curls != len(inputs):
        fail("Containerfile contains a download without a supported exact checksum declaration")
    return {"apk_groups": package_groups, "downloads": downloads}, inputs


def _npm_declarations(root: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    """Read exact registry artifacts, including optional platform packages."""
    lock = json.loads((root / "package-lock.json").read_text(encoding="utf-8"))
    packages = lock.get("packages")
    if not isinstance(packages, dict):
        fail("package-lock.json must declare package records")
    inputs = []
    context = {}
    for name, record in sorted(packages.items()):
        if name == "":
            context[name] = record
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
                key=f"npm:{name}",
                url=url,
                sha256=checksum if algorithm == "sha256" else None,
                size=None,
                destination=f"downloads/npm/{url_key}-{filename}",
                purpose="npm-package",
                checksum=checksum,
                algorithm=algorithm,
            )
        )
        context[name] = {
            field: record[field]
            for field in (
                "version",
                "resolved",
                "integrity",
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
    return {"packages": context}, inputs


def _selected_declarations(root: Path) -> tuple[dict[str, Any], dict[str, Any], set[str]]:
    """Select the package producers reachable through current targets and profiles."""
    targets = {}
    platforms = {}
    packages = set(alpine_registration.COMMON_PACKAGES)
    for path in sorted((root / "targets").glob("*/target.toml")):
        target = load_toml(path)
        platform_name = target["platform"]
        if platform_name not in platforms:
            platforms[platform_name] = load_toml(
                root / "platforms" / platform_name / "platform.toml"
            )
        platform = platforms[platform_name]
        selected_profiles = ["default", *(["microsd-uboot"] if "microsd" in target else [])]
        profile_context = {}
        for profile in selected_profiles:
            declaration = load_toml(root / "profiles" / profile / "profile.toml")
            packages.update(declaration["rootfs"]["packages"])
            profile_context[profile] = {
                "packages": sorted(declaration["rootfs"]["packages"]),
                "bootstrap": declaration["bootstrap"]["kind"],
                "uboot": declaration["uboot"]["kind"],
            }
        for declaration in (platform, target):
            for layer in ("rootfs", "bundle"):
                packages.update(declaration[layer]["packages"])
        targets[path.parent.name] = {
            "platform": platform_name,
            "rootfs": sorted(target["rootfs"]["packages"]),
            "bundle": sorted(target["bundle"]["packages"]),
            "profiles": profile_context,
        }
    if not targets:
        fail("dependency inventory requires at least one declared target")
    return targets, platforms, packages


def _source_input(
    key: str,
    source: dict[str, Any],
    destination: str,
    purpose: str,
) -> DependencyInput:
    return DependencyInput(
        key=key,
        url=source["archive_url"],
        sha256=source["archive_sha256"],
        size=None,
        destination=destination,
        purpose=purpose,
    )


def _environment_lock(
    root: Path, container_context: dict[str, object]
) -> tuple[dict[str, Any], list[DependencyInput]]:
    """Load the exact package closure selected for the build environment."""
    lock = load_toml(root / "environment.lock.toml")
    if set(lock) != {"selection", "input", "key"}:
        fail("environment lock must contain selection, input and key declarations")
    selection = lock["selection"]
    if not isinstance(selection, dict) or set(selection) != {"arch", "apk_groups"}:
        fail("environment lock selection must contain arch and apk_groups")
    if selection["arch"] != "x86_64":
        fail("environment lock must select x86_64")
    if selection["apk_groups"] != container_context["apk_groups"]:
        fail("environment lock APK selection differs from Containerfile")
    records = lock["input"]
    if not isinstance(records, list) or not records:
        fail("environment lock must declare external inputs")
    inputs = []
    for record in records:
        if not isinstance(record, dict) or set(record) not in (
            {"key", "url", "sha256", "bytes", "destination", "purpose", "arch"},
            {"key", "url", "sha256", "bytes", "destination", "purpose", "arch", "package"},
        ):
            fail("environment lock input has unsupported fields")
        inputs.append(
            DependencyInput(
                key=record["key"],
                url=record["url"],
                sha256=record["sha256"],
                size=record["bytes"],
                destination=record["destination"],
                purpose=record["purpose"],
                arch=record["arch"],
            )
        )
    normalized = {
        **lock,
        "input": sorted(records, key=lambda record: record["key"]),
        "key": sorted(lock["key"], key=lambda record: record["file"]),
    }
    return normalized, inputs


def _environment_declarations(root: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    container_lock = load_toml(root / "container.lock.toml")
    container_context, inputs = _container_declarations(root)
    npm_context, npm_inputs = _npm_declarations(root)
    lock_context, lock_inputs = _environment_lock(root, container_context)
    inputs.extend(npm_inputs)
    inputs.extend(lock_inputs)
    inputs.extend(
        [
            _source_input(
                "kern:archive",
                container_lock["kern"],
                "downloads/kern/kern.tar.gz",
                "kern-runtime",
            ),
            DependencyInput(
                key="container:base-rootfs",
                url=container_lock["oci"]["base_rootfs_url"],
                sha256=container_lock["oci"]["base_rootfs_sha256"],
                size=None,
                destination="downloads/kern/alpine-minirootfs.tar.gz",
                purpose="container-base",
                arch="x86_64",
            ),
        ]
    )
    return {
        "container_lock": container_lock,
        "container": container_context,
        "npm": npm_context,
        "environment_lock": lock_context,
    }, inputs


def environment_inputs(root: Path) -> list[DependencyInput]:
    """Return the inputs consumed by environment setup without loading target data."""
    return sorted(_environment_declarations(root)[1], key=lambda item: item.key)


def _inventory(root: Path) -> tuple[dict[str, object], list[DependencyInput]]:
    alpine = alpine_lock.load_alpine_lock(root)
    sources = load_toml(root / "sources.lock.toml")
    targets, platforms, packages = _selected_declarations(root)
    environment_context, inputs = _environment_declarations(root)
    inputs.extend(
        [
            DependencyInput(
                key="alpine:minirootfs",
                url=alpine["minirootfs"]["url"],
                sha256=alpine["minirootfs"]["sha256"],
                size=alpine["minirootfs"]["bytes"],
                destination=f"downloads/alpine/alpine-minirootfs-{alpine['release']}-{alpine['arch']}.tar.gz",
                purpose="target-base",
                arch=alpine["arch"],
            ),
        ]
    )
    for record in alpine["package"]:
        filename = record["file"]
        inputs.append(
            DependencyInput(
                key=f"alpine:{alpine['arch']}:{filename}",
                url=f"{alpine['repositories'][record['repository']]}/{filename}",
                sha256=record["sha256"],
                size=record["bytes"],
                destination=f"downloads/alpine/packages/{filename}",
                purpose="target-package",
                arch=alpine["arch"],
            )
        )
    platform_context = {}
    active_sources = {}
    for name, platform in sorted(platforms.items()):
        linux = platform["linux"]
        source_name = linux["source_lock"]
        source = sources[source_name]
        inputs.append(
            DependencyInput(
                key=f"source:{source_name}:linux",
                url=source["url"],
                sha256=source["sha256"],
                size=None,
                destination=f"downloads/linux/linux-{source['version']}.tar.xz",
                purpose="linux-source",
            )
        )
        active_sources[source_name] = source
        bootstrap = platform["bootstrap"]
        source_name = bootstrap["vendor_source_lock"]
        inputs.append(
            _source_input(
                f"source:{source_name}:bootstrap",
                sources[source_name],
                f"downloads/{bootstrap['vendor_cache_name']}",
                "bootstrap-source",
            )
        )
        active_sources[source_name] = sources[source_name]
        host_context = []
        for tool in platform["host"]["tools"]:
            if tool["type"] != "make-archive":
                continue
            source_name = tool["source_lock"]
            inputs.append(
                _source_input(
                    f"source:{source_name}:host",
                    sources[source_name],
                    f"downloads/{tool['cache_name']}",
                    "host-source",
                )
            )
            active_sources[source_name] = sources[source_name]
            host_context.append(
                {
                    field: tool[field]
                    for field in ("name", "source_lock", "cache_name", "archive_prefix", "members")
                }
            )
        platform_context[name] = {
            "rootfs": sorted(platform["rootfs"]["packages"]),
            "bundle": sorted(platform["bundle"]["packages"]),
            "linux_source": linux["source_lock"],
            "bootstrap_source": bootstrap["vendor_source_lock"],
            "bootstrap_cache": bootstrap["vendor_cache_name"],
            "host": host_context,
        }
        if any(
            selection["profiles"].get("microsd-uboot", {}).get("uboot") == "full"
            for selection in targets.values()
            if selection["platform"] == name
        ):
            lock = load_toml(root / platform["uboot"]["source"])
            inputs.append(
                _source_input(
                    f"uboot:{name}", lock, "downloads/uboot/source.tar.bz2", "uboot-source"
                )
            )
            platform_context[name]["uboot"] = lock
    asset_context = {}
    for target in targets:
        assets = load_asset_lock(root / "targets" / target / "loader/assets.lock.toml")
        asset_context[target] = assets
        for asset in assets:
            inputs.append(
                DependencyInput(
                    key=f"asset:{target}:{asset['id']}",
                    url=asset["url"],
                    sha256=asset["sha256"],
                    size=None,
                    destination=f"downloads/{asset['cache_name']}",
                    purpose="loader-asset",
                )
            )
    aport_context = {}
    producers = sorted(alpine_selection.aport_build_order(sorted(packages)))
    for producer in producers:
        declaration, source_inputs = _apkbuild_declarations(
            root / "alpine/aports" / producer / "APKBUILD"
        )
        aport_context[producer] = declaration
        inputs.extend(source_inputs)
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
        "targets": targets,
        "platforms": platform_context,
        "assets": asset_context,
        "producers": producers,
        "aports": aport_context,
    }
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
    return context, [unique[key] for key in sorted(unique)]


def dependency_selection(root: Path) -> tuple[list[DependencyInput], dict[str, object]]:
    """Return exact downloads and their semantic selection from one declaration inventory."""
    context, inputs = _inventory(root)
    return inputs, {**context, "inputs": [asdict(item) for item in inputs]}

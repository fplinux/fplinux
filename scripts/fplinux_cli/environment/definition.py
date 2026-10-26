# SPDX-License-Identifier: GPL-2.0-only
"""Identify the pinned build environment and its causal inputs."""

from __future__ import annotations

import hashlib
import re
from dataclasses import asdict
from typing import TYPE_CHECKING, Any, Protocol

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import resolve_source, source_build_metadata
from fplinux_cli.manifests.values import sha256_value

if TYPE_CHECKING:
    from pathlib import Path


ENVIRONMENT_RECIPE_INPUTS = (
    "scripts/fplinux_cli/environment/setup.py",
    "scripts/fplinux_cli/environment/definition.py",
    "scripts/fplinux_cli/environment/runtime.py",
    "scripts/fplinux_cli/environment/state.py",
    "scripts/fplinux_cli/environment/kern.py",
    "scripts/fplinux_cli/environment/downloads.py",
    "scripts/fplinux_cli/environment/rootfs.py",
    "scripts/fplinux_cli/environment/install.py",
    "scripts/fplinux_cli/environment/install/prepare.sh",
    "scripts/fplinux_cli/environment/image_content.py",
    "scripts/fplinux_cli/environment/install_wheels.py",
    "scripts/fplinux_cli/common.py",
    "scripts/fplinux_cli/dependencies/inputs.py",
    "scripts/fplinux_cli/manifests/source_catalog.py",
    "scripts/fplinux_cli/alpine/sources.py",
    "scripts/fplinux_cli/alpine/selection.py",
    "scripts/fplinux_cli/alpine/registration.py",
    "scripts/fplinux_cli/alpine/lock.py",
    "scripts/fplinux_cli/alpine/aports.py",
    "scripts/fplinux_cli/build/host_tools/host.py",
    "package.json",
    "package-lock.json",
    "environment.lock.toml",
    "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch",
    "alpine/aports/fplinux-font-terminus/APKBUILD",
)


def _resolve_environment_sources(lock: dict[str, Any], root: Path) -> dict[str, Any]:
    """Resolve direct originals while preserving the native binary APK closure."""
    kern = lock["kern"]
    if not isinstance(kern, dict) or set(kern) != {"source", "binary_sha256"}:
        fail("environment Kern must contain source and binary_sha256")
    kern_source = resolve_source(root, kern["source"])
    lock["kern"] = {
        "version": kern_source.version,
        "archive_url": kern_source.url,
        "archive_sha256": kern_source.checksum,
        "binary_sha256": kern["binary_sha256"],
        "destination": kern_source.destination,
    }
    base = lock["base"]
    if not isinstance(base, dict) or set(base) != {"source", "arch"}:
        fail("environment base must contain source and arch")
    base_source = resolve_source(root, base["source"])
    lock["base"] = {
        "release": base_source.version,
        "arch": base["arch"],
        "url": base_source.url,
        "sha256": base_source.checksum,
    }
    records = {}
    if not isinstance(lock["input"], list):
        fail("environment inputs must be an array")
    for record in lock["input"]:
        if not isinstance(record, dict):
            fail("environment input must be a table")
        if "source" in record:
            if set(record) != {"key", "source", "purpose", "arch"}:
                fail("environment source input must contain key, source, purpose and arch")
            source = resolve_source(root, record.pop("source"))
            record.update(url=source.url, destination=source.destination)
            if source.algorithm == "sha256":
                record["sha256"] = source.checksum
            else:
                record.update(algorithm=source.algorithm, checksum=source.checksum)
            if source.size is not None:
                record["bytes"] = source.size
            records[record["key"]] = source
        elif record.get("purpose") != "environment-package":
            fail("environment direct originals must use source references")
    if not isinstance(lock["tools"], dict):
        fail("environment tools must be a table")
    for name, tool in lock["tools"].items():
        if not isinstance(tool, dict) or set(tool) != {"input"} or tool["input"] not in records:
            fail(f"environment tool must select a direct source input: {name}")
        source = records[tool["input"]]
        tool["commit" if name == "sparse" else "version"] = source.version
    return lock


def load_environment_lock(root: Path | None = None) -> dict[str, Any]:
    """Load the exact runtime, rootfs, package selection and installation inputs."""
    if root is None:
        root = common.ROOT
    path = root / "environment.lock.toml"
    lock = common.load_toml(path)
    if set(lock) != {"kern", "base", "selection", "tools", "input"}:
        fail(f"environment lock must contain kern, base, selection, tools and input: {path}")
    lock = _resolve_environment_sources(lock, root)

    kern = lock.get("kern")
    if not isinstance(kern, dict) or set(kern) != {
        "version",
        "archive_url",
        "archive_sha256",
        "binary_sha256",
        "destination",
    }:
        fail(f"environment lock must define one pinned Kern release: {path}")
    version = kern.get("version")
    archive_url = kern.get("archive_url")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        fail(f"Kern version is invalid: {path}")
    if not isinstance(archive_url, str) or not archive_url.startswith("https://"):
        fail(f"Kern release URL must use HTTPS: {path}")
    for field in ("archive_sha256", "binary_sha256"):
        sha256_value(kern.get(field), f"Kern {field} in {path}")

    base = lock.get("base")
    if not isinstance(base, dict) or set(base) != {"release", "arch", "url", "sha256"}:
        fail(f"environment lock must define one exact Alpine rootfs: {path}")
    if base.get("arch") != "x86_64":
        fail(f"unsupported environment architecture: {path}")
    release = base.get("release")
    rootfs_url = base.get("url")
    rootfs_sha256 = base.get("sha256")
    if not isinstance(release, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", release) is None:
        fail(f"environment base release is invalid: {path}")
    if not isinstance(rootfs_url, str) or not rootfs_url.startswith("https://"):
        fail(f"environment base rootfs URL must use HTTPS: {path}")
    sha256_value(rootfs_sha256, f"environment base rootfs SHA-256 in {path}")
    selection = lock.get("selection")
    if not isinstance(selection, dict) or set(selection) != {"arch", "apk_groups"}:
        fail(f"environment selection must contain arch and apk_groups: {path}")
    if selection["arch"] != base["arch"] or not isinstance(selection["apk_groups"], list):
        fail(f"invalid environment package selection: {path}")
    for group in selection["apk_groups"]:
        if not isinstance(group, list) or not group:
            fail(f"invalid environment package group: {path}")
        for package in group:
            if (
                not isinstance(package, str)
                or re.fullmatch(r"[a-z0-9+_.-]+=[^\s=]+", package) is None
            ):
                fail(f"environment packages must have exact versions: {path}")
    records = lock.get("input")
    if not isinstance(records, list) or not records:
        fail(f"environment inputs are missing: {path}")
    keys: set[str] = set()
    for record in records:
        if not isinstance(record, dict):
            fail(f"invalid environment input: {path}")
        required = {"key", "url", "destination", "purpose", "arch"}
        optional = {"sha256", "checksum", "algorithm", "bytes"}
        if not required <= record.keys() or record.keys() - required - optional:
            fail(f"unsupported environment input fields: {path}")
        key = record["key"]
        if not isinstance(key, str) or not key or key in keys:
            fail(f"duplicate or invalid environment input key: {path}")
        keys.add(key)
        if record.get("sha256") is not None:
            sha256_value(record["sha256"], f"environment input {key}")
            if "checksum" in record or "algorithm" in record:
                fail(f"environment input declares two checksum owners: {key}")
        elif (
            record.get("algorithm") != "sha512"
            or not isinstance(record.get("checksum"), str)
            or re.fullmatch(r"[0-9a-f]{128}", record["checksum"]) is None
        ):
            fail(f"environment input needs an exact checksum: {key}")
        size = record.get("bytes")
        if size is not None and (not isinstance(size, int) or isinstance(size, bool) or size <= 0):
            fail(f"environment input size is invalid: {key}")
        if not isinstance(record["url"], str) or not record["url"].startswith("https://"):
            fail(f"environment input URL must use HTTPS: {key}")
        destination = record["destination"]
        if (
            not isinstance(destination, str)
            or not destination.startswith("downloads/")
            or ".." in destination.split("/")
        ):
            fail(f"environment input cache path is invalid: {key}")
        if record["arch"] != base["arch"]:
            fail(f"environment input architecture is invalid: {key}")
    tools = lock.get("tools")
    names = {
        "node",
        "npm",
        "ruff",
        "sparse",
        "typos",
        "vale-google",
        "libtsm",
        "lvgl",
        "weston",
        "mesa-headers",
        "gitleaks",
    }
    if not isinstance(tools, dict) or set(tools) - names:
        fail(f"environment tool metadata is incomplete: {path}")
    for name, tool in tools.items():
        identity = "commit" if name == "sparse" else "version"
        if not isinstance(tool, dict) or set(tool) != {identity, "input"}:
            fail(f"environment tool metadata is invalid: {name}")
        if not isinstance(tool[identity], str) or not tool[identity] or tool["input"] not in keys:
            fail(f"environment tool input is missing: {name}")
    _require_runtime_inputs(lock)
    return lock


def _require_runtime_inputs(lock: dict[str, Any]) -> None:
    """Keep runtime download metadata consistent with its explicit inventory entry."""
    for purpose, url, digest in (
        ("kern-runtime", lock["kern"]["archive_url"], lock["kern"]["archive_sha256"]),
        ("environment-base", lock["base"]["url"], lock["base"]["sha256"]),
    ):
        selected = [record for record in lock["input"] if record["purpose"] == purpose]
        if len(selected) != 1 or selected[0]["url"] != url or selected[0].get("sha256") != digest:
            fail(f"environment input does not match its {purpose} declaration")


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


def file_recipe_digest(paths: list[Path], *, prefix: bytes = b"") -> str:
    """Hash exact logical paths, bytes and modes for one causal file closure."""
    value = hashlib.sha256()
    _length_prefixed(value, prefix)
    for path in paths:
        contents, mode = _stable_regular_file(path)
        relative = path.relative_to(common.ROOT).as_posix()
        _length_prefixed(value, relative.encode())
        _length_prefixed(value, contents)
        _length_prefixed(value, mode.to_bytes(2, "big"))
    return value.hexdigest()


def spreadtrum_flash_environment_source(root: Path) -> dict[str, Any]:
    """Select the pinned source files needed by the offline RAM uploader tests."""
    source = source_build_metadata(
        "spreadtrum_flash", common.load_toml(root / "sources.lock.toml").get("spreadtrum_flash")
    )
    commit = source.get("commit")
    if not isinstance(commit, str) or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        fail("quality environment spreadtrum_flash commit is invalid")
    url = source.get("archive_url")
    if not isinstance(url, str) or not url.startswith("https://"):
        fail("quality environment spreadtrum_flash archive URL must use HTTPS")
    files = source.get("files")
    if not isinstance(files, dict):
        fail("quality environment spreadtrum_flash file checksums are missing")
    members = [
        {"path": "Makefile", "digest_key": "makefile_sha256"},
        {"path": "spd_dump.c", "digest_key": "spd_dump_c_sha256"},
        {"path": "spd_cmd.h", "digest_key": "spd_cmd_h_sha256"},
    ]
    return {
        "archive_url": url,
        "archive_sha256": sha256_value(
            source.get("archive_sha256"), "quality environment spreadtrum_flash archive"
        ),
        "commit": commit,
        "members": members,
        "files": {
            member["digest_key"]: sha256_value(
                files.get(member["digest_key"]), f"quality environment source {member['path']}"
            )
            for member in members
        },
    }


def environment_recipe_digest(lock: dict[str, Any] | None = None) -> str:
    """Hash installation code, resources, exact input declarations and runtime bytes."""
    # Dependency declarations use this module's lock validation during initialization.
    from fplinux_cli.dependencies.inputs import (  # noqa: PLC0415
        terminus_font_environment_inputs,
        ui_font_environment_inputs,
        ui_icon_environment_inputs,
    )

    if lock is None:
        lock = load_environment_lock()
    return file_recipe_digest(
        [common.ROOT / relative for relative in ENVIRONMENT_RECIPE_INPUTS],
        prefix=(
            b"fplinux.environment-recipe\0"
            + common.canonical_json_bytes(lock)
            + b"fplinux.quality-source-projection\0"
            + common.canonical_json_bytes(spreadtrum_flash_environment_source(common.ROOT))
            + b"fplinux.quality-ui-font-projection\0"
            + common.canonical_json_bytes(
                [asdict(item) for item in ui_font_environment_inputs(common.ROOT)]
            )
            + b"fplinux.quality-terminal-font-projection\0"
            + common.canonical_json_bytes(
                [asdict(item) for item in terminus_font_environment_inputs(common.ROOT)]
            )
            + b"fplinux.quality-ui-icon-projection\0"
            + common.canonical_json_bytes(
                [asdict(item) for item in ui_icon_environment_inputs(common.ROOT)]
            )
        ),
    )


def environment_artifact_recipe_digest(recipe: str, content: str) -> str:
    """Bind artifact recipes to the declared recipe and actual installed environment content."""
    digest = hashlib.sha256()
    digest.update(b"fplinux.environment-artifact-recipe\0")
    for item in (recipe, content):
        _length_prefixed(digest, bytes.fromhex(item))
    return digest.hexdigest()

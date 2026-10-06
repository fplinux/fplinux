# SPDX-License-Identifier: GPL-2.0-only
"""Validate the locked Alpine runtime and cross-build inputs."""

from __future__ import annotations

import re
import tomllib
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import ROOT, fail

if TYPE_CHECKING:
    from collections.abc import Sequence

PACKAGE_ID = re.compile(r"[a-z0-9][a-z0-9+._-]*")


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _sha256(value: object, name: str) -> str:
    if not _is_sha256(value):
        fail(f"{name} must be a lowercase SHA-256 digest")
    return str(value)


def _nonempty(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        fail(f"{name} must be a non-empty string")
    return value


def _https(value: object, name: str) -> str:
    result = _nonempty(value, name)
    if not result.startswith("https://"):
        fail(f"{name} must use HTTPS")
    return result


def _positive_integer(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        fail(f"{name} must be a positive integer")
    return value


def _package_name(value: object, name: str) -> str:
    result = _nonempty(value, name)
    path = PurePosixPath(result)
    if path.name != result or not result.endswith(".apk"):
        fail(f"{name} must be one APK filename")
    return result


def _package_id(value: object, name: str) -> str:
    result = _nonempty(value, name)
    if PACKAGE_ID.fullmatch(result) is None:
        fail(f"{name} must be one package identifier")
    return result


def load_alpine_lock(root: Path = ROOT) -> dict[str, Any]:
    """Load and validate the complete locked Alpine runtime/sysroot input set."""
    path = root / "alpine.lock.toml"
    try:
        with path.open("rb") as stream:
            raw = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        fail(f"cannot load {path}: {error}")
    if set(raw) != {
        "release",
        "branch",
        "arch",
        "triplet",
        "repositories",
        "minirootfs",
        "runtime",
        "sysroot",
        "package",
    }:
        fail(f"invalid Alpine lock: {path}")
    if raw.get("arch") != "armv7" or raw.get("triplet") != "armv7-alpine-linux-musleabihf":
        fail("only the FPLinux armv7 ABI is supported")
    _nonempty(raw.get("release"), "release")
    _nonempty(raw.get("branch"), "branch")
    repositories = raw.get("repositories")
    if not isinstance(repositories, dict) or set(repositories) != {"main", "community"}:
        fail("repositories must contain exactly main and community")
    for name, url in repositories.items():
        _https(url, f"{name} repository")

    minirootfs = raw.get("minirootfs")
    if not isinstance(minirootfs, dict) or set(minirootfs) != {"url", "sha256", "bytes"}:
        fail("minirootfs must contain exactly url, sha256 and bytes")
    _https(minirootfs.get("url"), "minirootfs URL")
    _sha256(minirootfs.get("sha256"), "minirootfs")
    _positive_integer(minirootfs.get("bytes"), "minirootfs bytes")

    packages = raw.get("package")
    if not isinstance(packages, list) or not packages:
        fail("package lock must be a non-empty array")
    records: dict[str, dict[str, object]] = {}
    for index, value in enumerate(packages):
        if not isinstance(value, dict) or set(value) != {
            "repository",
            "file",
            "sha256",
            "bytes",
        }:
            fail(f"package[{index}] must contain repository, file, sha256 and bytes")
        repository = value.get("repository")
        if repository not in repositories:
            fail(f"package[{index}] references an unknown repository")
        filename = _package_name(value.get("file"), f"package[{index}] file")
        if filename in records:
            fail(f"duplicate package lock entry: {filename}")
        _sha256(value.get("sha256"), f"package {filename}")
        _positive_integer(value.get("bytes"), f"package {filename} bytes")
        records[filename] = value

    selected: set[str] = set()

    def locked_names(value: object, name: str) -> list[str]:
        if not isinstance(value, list) or not value:
            fail(f"{name} must be a non-empty array")
        result = [_package_name(item, f"{name}[{index}]") for index, item in enumerate(value)]
        if len(result) != len(set(result)):
            fail(f"duplicate {name} package")
        for filename in result:
            if filename not in records:
                fail(f"{name} package has no locked artifact: {filename}")
        return result

    runtime = raw.get("runtime")
    if not isinstance(runtime, dict) or set(runtime) != {"packages", "additions"}:
        fail("runtime must contain exactly packages and additions")
    runtime_names = locked_names(runtime.get("packages"), "runtime packages")
    selected.update(runtime_names)
    additions = runtime.get("additions")
    if not isinstance(additions, dict):
        fail("runtime additions must be a table")
    for package, values in additions.items():
        package_name = _package_id(package, "runtime addition")
        addition_names = locked_names(values, f"runtime addition {package_name}")
        overlap = set(runtime_names) & set(addition_names)
        if overlap:
            fail(f"runtime addition {package_name} repeats a common runtime package")
        selected.update(addition_names)

    for group in ("sysroot",):
        table = raw.get(group)
        if not isinstance(table, dict) or set(table) != {"packages"}:
            fail(f"{group} must contain exactly packages")
        values = table.get("packages")
        selected.update(locked_names(values, f"{group} packages"))
    if selected != set(records):
        unused = ", ".join(sorted(set(records) - selected))
        fail(f"package lock contains unused artifacts: {unused}")
    return raw


def package_records(lock: dict[str, Any]) -> dict[str, dict[str, object]]:
    """Index already-validated locked package records by filename."""
    return {str(record["file"]): record for record in lock["package"]}


def runtime_package_names(lock: dict[str, Any], packages: Sequence[str]) -> tuple[str, ...]:
    """Select the common runtime closure plus additions required by local APKs."""
    names = list(lock["runtime"]["packages"])
    additions = lock["runtime"]["additions"]
    for package in packages:
        names.extend(additions.get(package, ()))
    return tuple(dict.fromkeys(names))

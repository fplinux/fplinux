# SPDX-License-Identifier: GPL-2.0-only
"""Causal Alpine rootfs recipes and successful-build receipts."""

from __future__ import annotations

import hashlib
import json
import re
import tempfile
import tomllib
from collections.abc import Mapping, Sequence
from pathlib import Path, PurePosixPath
from typing import Any, NoReturn

from .common import ROOT, sha256_file

RECEIPT_NAME = ".fplinux-rootfs-receipt.json"
ROOTFS_NAME = "rootfs.cpio"
SIGNING_KEY_DIRECTORY = "apk-signing"
SIGNING_PRIVATE_KEY = "fplinux-build.rsa"
SIGNING_PUBLIC_KEY = "fplinux-build.rsa.pub"
PACKAGE_CACHE_DIRECTORY = "apks"
PACKAGE_RECEIPT_NAME = ".fplinux-package-receipt.json"
SHARED_APORT_SOURCES = {
    "fplinux-console": (
        "alpine/shared/fplinux-multitap.c",
        "alpine/shared/fplinux-multitap.h",
    ),
    "fplinux-micropythonos": (
        "alpine/shared/fplinux-multitap.c",
        "alpine/shared/fplinux-multitap.h",
    ),
}
SHARED_APORT_SOURCE_PATHS = frozenset(
    path for paths in SHARED_APORT_SOURCES.values() for path in paths
)
COMMON_PACKAGES = (
    "fplinux-base",
    "fplinux-console",
    "fplinux-input",
)
PACKAGE_ID = re.compile(r"[a-z0-9][a-z0-9+._-]*")


def _fail(message: str) -> NoReturn:
    raise SystemExit(f"invalid Alpine rootfs state: {message}")


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _sha256(value: object, name: str) -> str:
    if not _is_sha256(value):
        _fail(f"{name} must be a lowercase SHA-256 digest")
    return str(value)


def _nonempty(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(f"{name} must be a non-empty string")
    return value


def _https(value: object, name: str) -> str:
    result = _nonempty(value, name)
    if not result.startswith("https://"):
        _fail(f"{name} must use HTTPS")
    return result


def _positive_integer(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        _fail(f"{name} must be a positive integer")
    return value


def _package_name(value: object, name: str) -> str:
    result = _nonempty(value, name)
    path = PurePosixPath(result)
    if path.name != result or not result.endswith(".apk"):
        _fail(f"{name} must be one APK filename")
    return result


def _package_id(value: object, name: str) -> str:
    result = _nonempty(value, name)
    if PACKAGE_ID.fullmatch(result) is None:
        _fail(f"{name} must be one package identifier")
    return result


def _declared_packages(config: Mapping[str, object], owner: str, layer: str) -> tuple[str, ...]:
    table = config.get(layer)
    if not isinstance(table, Mapping) or set(table) != {"packages"}:
        _fail(f"{owner} {layer} must contain exactly packages")
    raw = table.get("packages")
    if not isinstance(raw, list):
        _fail(f"{owner} {layer} packages must be an array")
    result = tuple(
        _package_id(package, f"{owner} {layer} packages[{index}]")
        for index, package in enumerate(raw)
    )
    if len(set(result)) != len(result):
        _fail(f"{owner} {layer} packages must not contain duplicates")
    return result


def _canonical_packages(packages: Sequence[str], root: Path) -> tuple[str, ...]:
    result = tuple(sorted(_package_id(package, "FPLinux package") for package in packages))
    if len(set(result)) != len(result):
        _fail("FPLinux package set must not contain duplicates")
    for package in result:
        aport = root / "alpine/aports" / package
        if aport.is_symlink() or not aport.is_dir():
            _fail(f"selected aport is missing or invalid: {package}")
        apkbuild = aport / "APKBUILD"
        if apkbuild.is_symlink() or not apkbuild.is_file():
            _fail(f"selected aport has no regular APKBUILD: {package}")
    return result


def selected_packages(
    platform_config: Mapping[str, object],
    target_config: Mapping[str, object],
    root: Path = ROOT,
) -> tuple[str, ...]:
    """Resolve the exact common, platform and target package ownership layers."""
    owners: dict[str, str] = {}
    for owner, packages in (
        ("common", COMMON_PACKAGES),
        ("platform", _declared_packages(platform_config, "platform", "rootfs")),
        ("target", _declared_packages(target_config, "target", "rootfs")),
    ):
        for package in packages:
            previous = owners.get(package)
            if previous is not None:
                _fail(f"package {package} is owned by both {previous} and {owner}")
            owners[package] = owner
    return _canonical_packages(tuple(owners), root)


def bundle_packages(
    platform_config: Mapping[str, object],
    target_config: Mapping[str, object],
    rootfs_packages: Sequence[str],
    root: Path = ROOT,
) -> tuple[str, ...]:
    """Resolve platform and target APKs published alongside, not in, the rootfs."""
    owners: dict[str, str] = {}
    for owner, packages in (
        ("platform", _declared_packages(platform_config, "platform", "bundle")),
        ("target", _declared_packages(target_config, "target", "bundle")),
    ):
        for package in packages:
            previous = owners.get(package)
            if previous is not None:
                _fail(f"bundle package {package} is owned by both {previous} and {owner}")
            owners[package] = owner
    result = _canonical_packages(tuple(owners), root)
    overlap = set(result) & set(rootfs_packages)
    if overlap:
        _fail(
            "packages cannot be both rootfs-selected and bundle-published: "
            + ", ".join(sorted(overlap))
        )
    return result


def load_alpine_lock(root: Path = ROOT) -> dict[str, Any]:
    """Load and validate the complete locked Alpine runtime/sysroot input set."""
    path = root / "alpine.lock.toml"
    try:
        with path.open("rb") as stream:
            raw = tomllib.load(stream)
    except (OSError, tomllib.TOMLDecodeError) as error:
        _fail(f"cannot load {path}: {error}")
    if (
        set(raw)
        != {
            "schema",
            "release",
            "branch",
            "arch",
            "triplet",
            "repository",
            "minirootfs",
            "runtime",
            "sysroot",
            "package",
        }
        or raw.get("schema") != "fplinux.alpine/v1"
    ):
        _fail(f"invalid schema: {path}")
    if raw.get("arch") != "armv7" or raw.get("triplet") != "armv7-alpine-linux-musleabihf":
        _fail("only the FPLinux armv7 ABI is supported")
    _nonempty(raw.get("release"), "release")
    _nonempty(raw.get("branch"), "branch")
    _https(raw.get("repository"), "repository")

    minirootfs = raw.get("minirootfs")
    if not isinstance(minirootfs, dict) or set(minirootfs) != {"url", "sha256", "bytes"}:
        _fail("minirootfs must contain exactly url, sha256 and bytes")
    _https(minirootfs.get("url"), "minirootfs URL")
    _sha256(minirootfs.get("sha256"), "minirootfs")
    _positive_integer(minirootfs.get("bytes"), "minirootfs bytes")

    packages = raw.get("package")
    if not isinstance(packages, list) or not packages:
        _fail("package lock must be a non-empty array")
    records: dict[str, dict[str, object]] = {}
    for index, value in enumerate(packages):
        if not isinstance(value, dict) or set(value) != {"file", "sha256", "bytes"}:
            _fail(f"package[{index}] must contain exactly file, sha256 and bytes")
        filename = _package_name(value.get("file"), f"package[{index}] file")
        if filename in records:
            _fail(f"duplicate package lock entry: {filename}")
        _sha256(value.get("sha256"), f"package {filename}")
        _positive_integer(value.get("bytes"), f"package {filename} bytes")
        records[filename] = value

    selected: set[str] = set()
    for group in ("runtime", "sysroot"):
        table = raw.get(group)
        if not isinstance(table, dict) or set(table) != {"packages"}:
            _fail(f"{group} must contain exactly packages")
        values = table.get("packages")
        if not isinstance(values, list) or not values:
            _fail(f"{group} packages must be a non-empty array")
        group_names: set[str] = set()
        for index, value in enumerate(values):
            filename = _package_name(value, f"{group} packages[{index}]")
            if filename in group_names:
                _fail(f"duplicate {group} package: {filename}")
            if filename not in records:
                _fail(f"{group} package has no locked artifact: {filename}")
            group_names.add(filename)
            selected.add(filename)
    if selected != set(records):
        unused = ", ".join(sorted(set(records) - selected))
        _fail(f"package lock contains unused artifacts: {unused}")
    return raw


def package_records(lock: dict[str, Any]) -> dict[str, dict[str, object]]:
    """Index already-validated locked package records by filename."""
    return {str(record["file"]): record for record in lock["package"]}


def _canonical_digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _source_file(path: Path, root: Path) -> dict[str, object]:
    if path.is_symlink() or not path.is_file():
        _fail(f"recipe input is missing or invalid: {path}")
    return {
        "path": path.relative_to(root).as_posix(),
        "sha256": sha256_file(path),
        "mode": path.stat().st_mode & 0o777,
    }


def _source_tree(path: Path, root: Path) -> list[dict[str, object]]:
    if path.is_symlink() or not path.is_dir():
        _fail(f"recipe tree is missing or invalid: {path}")
    entries: list[dict[str, object]] = []
    for child in sorted(path.rglob("*")):
        if child.is_symlink():
            _fail(f"recipe tree must not contain symlinks: {child}")
        if child.is_dir():
            continue
        entries.append(_source_file(child, root))
    return entries


def shared_aport_sources(package: str, root: Path = ROOT) -> tuple[Path, ...]:
    """Return canonical project sources copied into one consuming aport."""
    name = _package_id(package, "FPLinux package")
    return tuple(root / relative for relative in SHARED_APORT_SOURCES.get(name, ()))


def shared_aport_source_records(
    packages: Sequence[str], root: Path = ROOT
) -> list[dict[str, object]]:
    """Describe each canonical shared source used by the given package set once."""
    sources = {source for package in packages for source in shared_aport_sources(package, root)}
    return [_source_file(source, root) for source in sorted(sources)]


def signing_public_key(cache: Path) -> Path:
    """Return the persistent local abuild public key path."""
    return cache / SIGNING_KEY_DIRECTORY / SIGNING_PUBLIC_KEY


def signing_key_identity(cache: Path) -> str:
    """Return the SHA-256 identity of the persistent local abuild key."""
    path = signing_public_key(cache)
    if path.is_symlink() or not path.is_file():
        _fail(f"package signing public key is missing or invalid: {path}")
    return sha256_file(path)


def alpine_rootfs_recipe(
    container_image_recipe: str,
    signing_key_sha256: str,
    packages: Sequence[str],
    root: Path = ROOT,
) -> str:
    """Hash every input that can affect one selected Alpine root filesystem."""
    _sha256(container_image_recipe, "container image recipe")
    _sha256(signing_key_sha256, "package signing public key")
    selected = _canonical_packages(packages, root)
    payload = {
        "container_image_recipe": container_image_recipe,
        "package_signing_key": signing_key_sha256,
        "packages": list(selected),
        "lock": _source_file(root / "alpine.lock.toml", root),
        "abuild": _source_file(root / "alpine/abuild.conf", root),
        "aports": {name: _source_tree(root / "alpine/aports" / name, root) for name in selected},
        "shared_aport_sources": shared_aport_source_records(selected, root),
        "implementation": [
            _source_file(root / "scripts/fplinux_cli/alpine_state.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine_builder.py", root),
        ],
    }
    return _canonical_digest(payload)


def alpine_package_recipe(
    name: str,
    container_image_recipe: str,
    signing_key_sha256: str,
    root: Path = ROOT,
) -> str:
    """Hash the inputs that can affect one current FPLinux APK."""
    name = _canonical_packages((name,), root)[0]
    _sha256(container_image_recipe, "container image recipe")
    _sha256(signing_key_sha256, "package signing public key")
    return _canonical_digest(
        {
            "container_image_recipe": container_image_recipe,
            "package_signing_key": signing_key_sha256,
            "lock": _source_file(root / "alpine.lock.toml", root),
            "abuild": _source_file(root / "alpine/abuild.conf", root),
            "aport": _source_tree(root / "alpine/aports" / name, root),
            "shared_aport_sources": shared_aport_source_records((name,), root),
            "implementation": [
                _source_file(root / "scripts/fplinux_cli/alpine_state.py", root),
                _source_file(root / "scripts/fplinux_cli/alpine_builder.py", root),
            ],
        }
    )


def rootfs_output(cache: Path, recipe: str) -> Path:
    """Return the immutable cache directory for one exact rootfs recipe."""
    _sha256(recipe, "Alpine rootfs recipe")
    return cache / "rootfs" / recipe


def _rootfs_record(path: Path) -> dict[str, int | str]:
    if path.is_symlink() or not path.is_file():
        _fail(f"rootfs output is missing or invalid: {path}")
    return {"sha256": sha256_file(path), "size": path.stat().st_size}


def _receipt_data(output: Path, recipe: str) -> dict[str, object]:
    return {
        "recipe": _sha256(recipe, "Alpine rootfs recipe"),
        "rootfs": _rootfs_record(output / ROOTFS_NAME),
    }


def _read_receipt(output: Path) -> dict[str, object] | None:
    try:
        raw = json.loads((output / RECEIPT_NAME).read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(raw, dict) or set(raw) != {"recipe", "rootfs"}:
        return None
    if not _is_sha256(raw.get("recipe")):
        return None
    rootfs = raw.get("rootfs")
    if (
        not isinstance(rootfs, dict)
        or set(rootfs) != {"sha256", "size"}
        or not _is_sha256(rootfs.get("sha256"))
        or not isinstance(rootfs.get("size"), int)
        or isinstance(rootfs.get("size"), bool)
        or int(rootfs["size"]) < 0
    ):
        return None
    return raw


def receipt_matches(output: Path, recipe: str) -> bool:
    """Return whether one success receipt and rootfs still match exactly."""
    raw = _read_receipt(output)
    if raw is None or raw.get("recipe") != recipe:
        return False
    try:
        return raw.get("rootfs") == _rootfs_record(output / ROOTFS_NAME)
    except SystemExit:
        return False


def write_receipt(output: Path, recipe: str) -> None:
    """Atomically publish a successful rootfs receipt after the cpio exists."""
    if output.is_symlink() or not output.is_dir():
        _fail(f"rootfs output directory is invalid: {output}")
    encoded = (json.dumps(_receipt_data(output, recipe), sort_keys=True) + "\n").encode()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=output, prefix=f".{RECEIPT_NAME}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(encoded)
        temporary.replace(output / RECEIPT_NAME)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def trusted_receipt_identity(output: Path, recipe: str) -> dict[str, str]:
    """Return the identity of an exact receipt whose rootfs still verifies."""
    if not receipt_matches(output, recipe):
        _fail("rootfs causal receipt is missing, stale or invalid")
    receipt = output / RECEIPT_NAME
    return {"recipe": recipe, "sha256": sha256_file(receipt)}

# SPDX-License-Identifier: GPL-2.0-only
"""Fetch locked Alpine packages and validate built APK cache outputs."""

from __future__ import annotations

import json
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

from fplinux_cli.build.inputs import CACHE, require_file, require_sha256
from fplinux_cli.build.process import run
from fplinux_cli.build.sources import fetch
from fplinux_cli.common import fail, sha256_file

from .lock import PACKAGE_ID, runtime_package_names
from .recipes import alpine_package_recipe

PACKAGE_CACHE_DIRECTORY = "apks"
PACKAGE_RECEIPT_NAME = ".fplinux-package-receipt.json"


def _locked_alpine_artifact(
    lock: dict[str, Any],
    records: dict[str, dict[str, object]],
    filename: str,
    *,
    cache: Path = CACHE,
) -> Path:
    record = records.get(filename)
    if record is None:
        fail(f"locked Alpine package is missing: {filename}")
    repository = record.get("repository")
    if not isinstance(repository, str):
        fail(f"locked Alpine package repository is invalid: {filename}")
    package = fetch(
        f"{lock['repositories'][repository]}/{filename}",
        record.get("sha256"),
        cache / "downloads/alpine/packages",
        filename,
    )
    expected_size = record.get("bytes")
    if package.stat().st_size != expected_size:
        fail(
            f"Alpine package size mismatch for {filename}: "
            f"expected {expected_size}, received {package.stat().st_size}"
        )
    return package


def _alpine_group_packages(
    lock: dict[str, Any],
    records: dict[str, dict[str, object]],
    group: str,
    *,
    cache: Path = CACHE,
) -> list[Path]:
    return [
        _locked_alpine_artifact(lock, records, filename, cache=cache)
        for filename in lock[group]["packages"]
    ]


def _alpine_runtime_packages(
    lock: dict[str, Any],
    records: dict[str, dict[str, object]],
    packages: tuple[str, ...],
) -> list[Path]:
    """Fetch only the locked Alpine closure needed by this rootfs selection."""
    return [
        _locked_alpine_artifact(lock, records, filename)
        for filename in runtime_package_names(lock, packages)
    ]


def _cached_package_files(repository: Path, names: set[str]) -> list[Path] | None:
    packages: list[Path] = []
    for name in sorted(names):
        matches = [path for path in repository.rglob(name) if path.is_file()]
        if len(matches) != 1:
            return None
        packages.append(matches[0])
    return packages


def _apk_metadata_lines(path: Path) -> list[str]:
    """Read package metadata used by artifact and solver decisions."""
    try:
        with tarfile.open(require_file(path), "r:*") as archive:
            metadata = archive.extractfile(".PKGINFO")
            if metadata is None:
                fail(f"Alpine package has no .PKGINFO: {path}")
            lines = metadata.read().decode("utf-8").splitlines()
    except (OSError, UnicodeDecodeError, tarfile.TarError) as error:
        fail(f"cannot read Alpine package metadata: {path}: {error}")
    return lines


def _apk_package_name(path: Path) -> str:
    """Read the exact package identity carried by one Alpine APK."""
    lines = _apk_metadata_lines(path)
    names = [line.removeprefix("pkgname = ") for line in lines if line.startswith("pkgname = ")]
    if len(names) != 1 or PACKAGE_ID.fullmatch(names[0]) is None:
        fail(f"Alpine package has an invalid pkgname: {path}")
    return names[0]


def _package_receipt_data(
    repository: Path, recipe: str, package_files: list[Path]
) -> dict[str, object]:
    """Describe the exact signed APK outputs in one aport cache slot."""
    packages: dict[str, dict[str, int | str]] = {}
    for path in sorted(package_files):
        package = _apk_package_name(path)
        if package in packages:
            fail(f"aport produced duplicate package identity: {package}")
        packages[package] = {
            "path": path.relative_to(repository).as_posix(),
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
    if not packages:
        fail("aport produced no APK packages")
    return {
        "recipe": require_sha256(recipe, "Alpine package recipe"),
        "packages": packages,
    }


def _write_package_receipt(repository: Path, recipe: str, package_files: list[Path]) -> None:
    """Publish a package success receipt only after every APK is in its cache slot."""
    receipt = repository / PACKAGE_RECEIPT_NAME
    encoded = (
        json.dumps(_package_receipt_data(repository, recipe, package_files), sort_keys=True) + "\n"
    ).encode()
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=repository, prefix=f".{receipt.name}.", delete=False
        ) as stream:
            temporary = Path(stream.name)
            stream.write(encoded)
        temporary.replace(receipt)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _cached_package_record(
    repository: Path, package: object, record: object
) -> tuple[str, Path] | None:
    """Validate one package entry from a current cache receipt."""
    if (
        not isinstance(package, str)
        or PACKAGE_ID.fullmatch(package) is None
        or not isinstance(record, dict)
        or set(record) != {"path", "sha256", "size"}
    ):
        return None
    relative = record.get("path")
    if not isinstance(relative, str):
        return None
    path = PurePosixPath(relative)
    output = repository / relative
    digest = record.get("sha256")
    size = record.get("size")
    if (
        path.is_absolute()
        or ".." in path.parts
        or path.as_posix() != relative
        or output.is_symlink()
        or not output.is_file()
        or output.suffix != ".apk"
        or not isinstance(size, int)
        or isinstance(size, bool)
        or size < 0
        or output.stat().st_size != size
        or not isinstance(digest, str)
        or sha256_file(output) != digest
        or _apk_package_name(output) != package
    ):
        return None
    return package, output


def _cached_aport_packages(
    name: str, image_recipe: str, signing_key_identity: str
) -> dict[str, Path] | None:
    """Return exact current APK outputs without invoking the cross-build sysroot."""
    recipe = alpine_package_recipe(name, image_recipe, signing_key_identity)
    repository = CACHE / PACKAGE_CACHE_DIRECTORY / name
    try:
        raw = json.loads((repository / PACKAGE_RECEIPT_NAME).read_text(encoding="utf-8"))
    except OSError, UnicodeDecodeError, json.JSONDecodeError:
        return None
    if not isinstance(raw, dict) or set(raw) != {"recipe", "packages"}:
        return None
    packages = raw.get("packages")
    if raw.get("recipe") != recipe or not isinstance(packages, dict) or name not in packages:
        return None
    result: dict[str, Path] = {}
    try:
        for package, record in packages.items():
            cached = _cached_package_record(repository, package, record)
            if cached is None:
                return None
            cached_package, output = cached
            result[cached_package] = output
    except SystemExit:
        return None
    return result


def alpine_sysroot_command(
    lock: dict[str, Any], packages: list[Path], sysroot: Path, keys: Path
) -> list[str]:
    """Install exact APK files without networking or target scripts."""
    # A local APK replaces a locked development package deliberately. Update
    # that world name in the same transaction, releasing its old file digest
    # while the new signed APK retains an exact file constraint of its own.
    try:
        world = (sysroot / "etc/apk/world").read_text(encoding="utf-8")
    except FileNotFoundError:
        world = ""
    except (OSError, UnicodeError) as error:
        fail(f"Alpine sysroot world cannot be read: {error}")
    installed_names = {
        match.group(0)
        for entry in world.splitlines()
        if (match := PACKAGE_ID.match(entry)) is not None
    }
    replaced_names: set[str] = set()
    if installed_names:
        for package in packages:
            for line in _apk_metadata_lines(package):
                if line.startswith("replaces = "):
                    match = PACKAGE_ID.match(line.removeprefix("replaces = "))
                    if match is not None and match.group(0) in installed_names:
                        replaced_names.add(match.group(0))
    return [
        "apk",
        "--root",
        str(sysroot),
        "--arch",
        lock["arch"],
        "--initdb",
        "--no-network",
        "--no-scripts",
        "--no-logfile",
        "--keys-dir",
        str(keys),
        "add",
        *sorted(replaced_names),
        *(str(package) for package in packages),
    ]


def _prepare_alpine_sysroot(
    lock: dict[str, Any], packages: list[Path], sysroot: Path, keys: Path
) -> None:
    run(alpine_sysroot_command(lock, packages, sysroot, keys))

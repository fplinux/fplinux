# SPDX-License-Identifier: GPL-2.0-only
"""Causal Buildroot recipes and successful-build receipts."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import shlex
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any

from .common import fail, sha256_file
from .toolchain_state import toolchain_external_defconfig

RECEIPT_NAME = ".fplinux-buildroot-receipt.json"


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


def _relative(value: str, name: str) -> str:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or value != path.as_posix():
        fail(f"{name} must be a normalized relative path: {value}")
    return value


def _source_file(path: Path, name: str) -> dict[str, object]:
    if not path.is_file():
        fail(f"Buildroot recipe input is missing: {name}")
    return {
        "path": name,
        "sha256": sha256_file(path),
        "mode": path.stat().st_mode & 0o777,
    }


def _source_tree(path: Path, name: str) -> list[dict[str, object]]:
    if not path.is_dir():
        fail(f"Buildroot recipe tree is missing: {name}")
    entries: list[dict[str, object]] = [
        {"path": name, "type": "directory", "mode": path.stat().st_mode & 0o777}
    ]
    for child in sorted(path.rglob("*")):
        identity = f"{name}/{child.relative_to(path).as_posix()}"
        if child.is_symlink():
            entries.append({"path": identity, "type": "symlink", "target": child.readlink()})
        elif child.is_dir():
            entries.append(
                {"path": identity, "type": "directory", "mode": child.stat().st_mode & 0o777}
            )
        elif child.is_file():
            entries.append(_source_file(child, identity))
        else:
            fail(f"Buildroot recipe input is not a file or directory: {identity}")
    return entries


def _defconfig_values(defconfig: Path, symbol: str) -> list[str]:
    prefix = f"{symbol}="
    values: list[str] = []
    for raw_line in defconfig.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line.startswith(prefix):
            continue
        try:
            parsed = shlex.split(line[len(prefix) :])
        except ValueError as error:
            fail(f"invalid {symbol} in {defconfig}: {error}")
        if len(parsed) != 1:
            fail(f"{symbol} in {defconfig} must have exactly one value")
        values.extend(parsed[0].split())
    return values


def _configured_workspace_trees(defconfig: Path, root: Path) -> list[tuple[str, Path]]:
    configured: dict[str, Path] = {}
    for symbol in ("BR2_ROOTFS_OVERLAY", "BR2_ROOTFS_POST_BUILD_SCRIPT"):
        for value in _defconfig_values(defconfig, symbol):
            if not value.startswith("/workspace/"):
                continue
            relative = _relative(value.removeprefix("/workspace/"), "Buildroot workspace path")
            configured[relative] = root / relative
    return sorted(configured.items())


def _identity_tree(path: Path, identity: str) -> dict[str, object]:
    if path.is_file():
        return {"identity": identity, "entries": [_source_file(path, identity)]}
    if path.is_dir():
        return {"identity": identity, "entries": _source_tree(path, identity)}
    return fail(f"Buildroot recipe input is missing: {identity}")


def _canonical_digest(value: dict[str, object]) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


@dataclasses.dataclass(frozen=True)
class BuildrootRecipe:
    """Split causal identity: one shared base plus per-package payloads."""

    base: str
    packages: dict[str, str]

    @property
    def combined(self) -> str:
        """Return the one digest covering every Buildroot input."""
        return _canonical_digest({"base": self.base, "packages": dict(self.packages)})


def _package_names(external: Path) -> list[str]:
    package_root = external / "package"
    if not package_root.is_dir():
        return []
    return sorted(child.name for child in package_root.iterdir() if child.is_dir())


def _is_package_payload(path: str, external_identity: str, names: list[str]) -> bool:
    for name in names:
        prefix = f"{external_identity}/package/{name}/"
        if path.startswith(prefix) and path != f"{prefix}Config.in":
            return True
    return False


def _package_payload_digest(external: Path, external_identity: str, name: str) -> str:
    identity = f"{external_identity}/package/{name}"
    entries = [
        entry
        for entry in _source_tree(external / "package" / name, identity)
        if entry["path"] != f"{identity}/Config.in"
    ]
    return _canonical_digest({"identity": identity, "entries": entries})


def buildroot_recipe(  # noqa: PLR0913, PLR0917 -- causal inputs remain separate.
    root: Path,
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    container_lock: dict[str, Any],
    container_image_recipe: str,
    toolchain_digest: str,
) -> BuildrootRecipe:
    """Hash the configured Buildroot source and configuration closure."""
    _sha256(toolchain_digest, "toolchain recipe")
    _sha256(container_image_recipe, "container image recipe")
    buildroot = container_lock.get("buildroot")
    if not isinstance(buildroot, dict):
        fail("container lock is missing Buildroot identity")
    version = buildroot.get("version")
    if not isinstance(version, str) or not version:
        fail("Buildroot version must be a non-empty string")
    buildroot_sha256 = _sha256(buildroot.get("sha256"), "Buildroot source")

    target_root = root / "targets" / target
    buildroot_config = target_config["buildroot"]
    platform_buildroot = platform["buildroot"]
    defconfig_relative = _relative(buildroot_config["defconfig"], "target Buildroot defconfig")
    defconfig = target_root / defconfig_relative
    if not defconfig.is_file():
        fail(f"target Buildroot defconfig is missing: {defconfig_relative}")
    external_relative = _relative(platform_buildroot["external"], "platform Buildroot external")

    external = root / external_relative
    names = _package_names(external)
    packages = {name: _package_payload_digest(external, external_relative, name) for name in names}

    configured_trees: dict[str, Path] = {external_relative: external}
    configured_trees.update(_configured_workspace_trees(defconfig, root))
    inputs: list[dict[str, object]] = []
    for identity, path in sorted(configured_trees.items()):
        if identity == external_relative:
            entries = [
                entry
                for entry in _source_tree(path, identity)
                if not _is_package_payload(str(entry["path"]), identity, names)
            ]
            inputs.append({"identity": identity, "entries": entries})
        else:
            inputs.append(_identity_tree(path, identity))
    manifest: dict[str, object] = {
        "container_image_recipe": container_image_recipe,
        "buildroot": {"version": version, "sha256": buildroot_sha256},
        "toolchain": {
            "recipe": toolchain_digest,
            "external_defconfig": sha256_file(toolchain_external_defconfig(root, platform)),
        },
        "defconfig": _source_file(defconfig, f"targets/{target}/{defconfig_relative}"),
        "inputs": inputs,
    }
    return BuildrootRecipe(base=_canonical_digest(manifest), packages=packages)


def buildroot_output_paths(platform: dict[str, Any]) -> tuple[str, str]:
    """Return the outputs required before Buildroot can be reused."""
    cross_compile = platform["linux"]["cross_compile"]
    if not isinstance(cross_compile, str) or not cross_compile:
        fail("platform Buildroot cross compiler prefix is invalid")
    return ("images/rootfs.cpio", f"host/bin/{cross_compile}gcc")


def _output_exists(output: Path, relative: str) -> bool:
    candidate = output / _relative(relative, "Buildroot output path")
    return candidate.is_file()


def _receipt_data(
    recipe: BuildrootRecipe, output: Path, outputs: tuple[str, ...]
) -> dict[str, object]:
    if len(outputs) != len(set(outputs)):
        fail("Buildroot output paths must not contain duplicates")
    for relative in outputs:
        if not _output_exists(output, relative):
            fail(f"Buildroot output is missing: {relative}")
    return {
        "recipe": recipe.combined,
        "base": recipe.base,
        "packages": dict(recipe.packages),
        "outputs": list(outputs),
    }


def _read_receipt(output: Path) -> object | None:
    try:
        value: object = json.loads((output / RECEIPT_NAME).read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None
    else:
        return value


def _valid_receipt(output: Path, outputs: tuple[str, ...]) -> dict[str, object] | None:
    if len(outputs) != len(set(outputs)):
        return None
    raw = _read_receipt(output)
    if (
        not isinstance(raw, dict)
        or set(raw) != {"recipe", "base", "packages", "outputs"}
        or not isinstance(raw.get("packages"), dict)
    ):
        return None
    if raw.get("outputs") != list(outputs):
        return None
    if not all(_output_exists(output, relative) for relative in outputs):
        return None
    return raw


def receipt_matches(output: Path, recipe: BuildrootRecipe, outputs: tuple[str, ...]) -> bool:
    """Return whether an exact successful receipt still matches its outputs."""
    raw = _valid_receipt(output, outputs)
    return (
        raw is not None
        and raw.get("recipe") == recipe.combined
        and raw.get("base") == recipe.base
        and raw.get("packages") == recipe.packages
    )


def receipt_digest_matches(output: Path, recipe: str, outputs: tuple[str, ...]) -> bool:
    """Return whether the receipt records exactly this combined recipe digest."""
    raw = _valid_receipt(output, outputs)
    return raw is not None and raw.get("recipe") == recipe


def stale_packages(
    output: Path, recipe: BuildrootRecipe, outputs: tuple[str, ...]
) -> tuple[str, ...] | None:
    """Return the packages a shared-base receipt can rebuild in place, else None."""
    raw = _valid_receipt(output, outputs)
    if raw is None or raw.get("base") != recipe.base:
        return None
    previous = raw.get("packages")
    if not isinstance(previous, dict) or set(previous) != set(recipe.packages):
        return None
    changed = tuple(
        name for name in sorted(recipe.packages) if previous[name] != recipe.packages[name]
    )
    return changed or None


def discard_success_receipt(output: Path) -> None:
    """Remove a prior success receipt before beginning a replacement build."""
    (output / RECEIPT_NAME).unlink(missing_ok=True)


def write_receipt(output: Path, recipe: BuildrootRecipe, outputs: tuple[str, ...]) -> None:
    """Atomically publish a receipt after all required outputs exist."""
    _sha256(recipe.base, "Buildroot base recipe")
    for name, digest in recipe.packages.items():
        _sha256(digest, f"Buildroot package recipe {name}")
    if not output.is_dir():
        fail(f"Buildroot output directory is invalid: {output}")
    encoded = (json.dumps(_receipt_data(recipe, output, outputs), sort_keys=True) + "\n").encode()
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

# SPDX-License-Identifier: GPL-2.0-only
"""Hash causal inputs for selected Alpine APK and rootfs outputs."""

from __future__ import annotations

import hashlib
import json
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import ROOT, fail, sha256_file

from .lock import _sha256
from .selection import (
    _canonical_packages,
    aport_build_order,
    aport_producer,
    selected_aport_graph,
    shared_aport_sources,
)

if TYPE_CHECKING:
    from collections.abc import Sequence
    from pathlib import Path

    from fplinux_cli.device_data.inputs import FirmwareInput


def _canonical_digest(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _source_file(path: Path, root: Path) -> dict[str, object]:
    if path.is_symlink() or not path.is_file():
        fail(f"recipe input is missing or invalid: {path}")
    return {
        "path": path.relative_to(root).as_posix(),
        "sha256": sha256_file(path),
        "mode": path.stat().st_mode & 0o777,
    }


def _source_tree(path: Path, root: Path) -> list[dict[str, object]]:
    if path.is_symlink() or not path.is_dir():
        fail(f"recipe tree is missing or invalid: {path}")
    entries: list[dict[str, object]] = []
    for child in sorted(path.rglob("*")):
        relative = child.relative_to(root)
        if "__pycache__" in relative.parts or child.suffix in {".pyc", ".pyo"}:
            continue
        if child.is_symlink():
            fail(f"recipe tree must not contain symlinks: {child}")
        if child.is_dir():
            continue
        entries.append(_source_file(child, root))
    return entries


def shared_aport_source_records(
    packages: Sequence[str], root: Path = ROOT
) -> list[dict[str, object]]:
    """Describe each canonical shared source used by the given package set once."""
    sources = {source for package in packages for source in shared_aport_sources(package, root)}
    return [_source_file(source, root) for source in sorted(sources)]


def alpine_rootfs_recipe(  # noqa: PLR0913 -- each selected rootfs input is causal.
    container_image_recipe: str,
    signing_key_sha256: str,
    packages: Sequence[str],
    root: Path = ROOT,
    *,
    firmware_inputs: Sequence[FirmwareInput] = (),
    display_brightness: dict[str, Any] | None = None,
    root_kind: str = "initramfs",
) -> str:
    """Hash every input that can affect one selected Alpine root filesystem."""
    if root_kind not in {"initramfs", "external"}:
        fail("Alpine root kind must be initramfs or external")
    _sha256(container_image_recipe, "container image recipe")
    _sha256(signing_key_sha256, "package signing public key")
    selected = _canonical_packages(packages, root)
    build_packages = _canonical_packages(aport_build_order(selected), root)
    payload = {
        "container_image_recipe": container_image_recipe,
        "package_signing_key": signing_key_sha256,
        "packages": list(selected),
        "registration": selected_aport_graph(selected),
        "lock": _source_file(root / "alpine.lock.toml", root),
        "abuild": _source_file(root / "alpine/abuild.conf", root),
        "aports": {
            name: _source_tree(root / "alpine/aports" / name, root) for name in build_packages
        },
        "shared_aport_sources": shared_aport_source_records(build_packages, root),
        "firmware": [firmware.recipe_record() for firmware in firmware_inputs],
        "display_brightness": display_brightness,
        "root_kind": root_kind,
        "ram_bootstrap": (
            _source_file(root / "alpine/ramroot-init.sh", root)
            if root_kind == "initramfs"
            else None
        ),
        "implementation": [
            _source_file(root / "scripts/fplinux_cli/alpine/lock.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/selection.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/recipes.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/signing.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/aports.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/packages.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/rootfs.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/rootfs_state.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/rootfs_verify.py", root),
            _source_file(root / "scripts/fplinux_cli/alpine/rootfs_files.py", root),
            _source_file(root / "scripts/fplinux_cli/common.py", root),
            _source_file(root / "scripts/fplinux_cli/build/environment.py", root),
            _source_file(root / "scripts/fplinux_cli/build/inputs.py", root),
            _source_file(root / "scripts/fplinux_cli/build/process.py", root),
            _source_file(root / "scripts/fplinux_cli/build/sources.py", root),
            _source_file(root / "scripts/fplinux_cli/device_data/inputs.py", root),
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
    name = aport_producer(_canonical_packages((name,), root)[0])
    graph = selected_aport_graph((name,))
    dependencies = _canonical_packages(
        tuple(producer for producer in graph if producer != name), root
    )
    _sha256(container_image_recipe, "container image recipe")
    _sha256(signing_key_sha256, "package signing public key")
    return _canonical_digest(
        {
            "container_image_recipe": container_image_recipe,
            "package_signing_key": signing_key_sha256,
            "producer": name,
            "registration": graph,
            "lock": _source_file(root / "alpine.lock.toml", root),
            "abuild": _source_file(root / "alpine/abuild.conf", root),
            "aport": _source_tree(root / "alpine/aports" / name, root),
            "local_build_dependencies": {
                library: _source_tree(root / "alpine/aports" / library, root)
                for library in dependencies
            },
            "shared_aport_sources": shared_aport_source_records((name, *dependencies), root),
            "implementation": [
                _source_file(root / "scripts/fplinux_cli/alpine/lock.py", root),
                _source_file(root / "scripts/fplinux_cli/alpine/selection.py", root),
                _source_file(root / "scripts/fplinux_cli/alpine/recipes.py", root),
                _source_file(root / "scripts/fplinux_cli/alpine/signing.py", root),
                _source_file(root / "scripts/fplinux_cli/alpine/aports.py", root),
                _source_file(root / "scripts/fplinux_cli/alpine/packages.py", root),
                _source_file(root / "scripts/fplinux_cli/common.py", root),
                _source_file(root / "scripts/fplinux_cli/build/environment.py", root),
                _source_file(root / "scripts/fplinux_cli/build/inputs.py", root),
                _source_file(root / "scripts/fplinux_cli/build/process.py", root),
                _source_file(root / "scripts/fplinux_cli/build/sources.py", root),
            ],
        }
    )

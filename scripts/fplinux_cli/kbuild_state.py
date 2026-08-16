# SPDX-License-Identifier: GPL-2.0-only
"""Exact Kbuild receipt for one stable ``work/kernel`` output directory."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import NoReturn

from .common import sha256_file

RECEIPT_NAME = ".fplinux-kbuild-receipt.json"
_ROOTFS_INPUT = "rootfs.cpio"


class KbuildStateError(ValueError):
    """Raised when the Kbuild receipt cannot describe the current output."""


def _error(message: str) -> NoReturn:
    raise KbuildStateError(message)


@dataclass(frozen=True)
class KbuildPlan:
    """The exact recipe and files required to reuse ``work/kernel``."""

    recipe: str
    rootfs: dict[str, int | str]
    rootfs_input: Path
    outputs: tuple[str, ...]


def _is_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def _require_digest(value: object, field: str) -> str:
    if not _is_sha256(value):
        _error(f"{field} must be a lowercase SHA-256 digest")
    return str(value)


def _relative(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        _error(f"{field} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or value != path.as_posix():
        _error(f"{field} must be a normalized relative path: {value!r}")
    return value


def _canonical_json(value: object) -> bytes:
    return (
        json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
        + b"\n"
    )


def _recipe_command(command: list[str]) -> list[str]:
    """Remove only the non-causal parallelism value from a Kbuild command."""
    return ["-j" if value.startswith("-j") and value[2:].isdigit() else value for value in command]


def _file_record(path: Path) -> dict[str, int | str]:
    try:
        if not path.is_file():
            _error(f"Kbuild file is missing or invalid: {path}")
        return {"sha256": sha256_file(path), "size": path.stat().st_size}
    except OSError as error:
        raise KbuildStateError(f"Kbuild file is missing or invalid: {path}") from error


def _file_record_or_none(path: Path) -> dict[str, int | str] | None:
    try:
        return _file_record(path)
    except KbuildStateError:
        return None


def _rootfs_record(rootfs: dict[str, int | str]) -> dict[str, int | str]:
    digest = _require_digest(rootfs.get("sha256"), "rootfs SHA-256")
    size = rootfs.get("size")
    if type(size) is not int or size < 0:
        _error("rootfs size must be a non-negative integer")
    return {"sha256": digest, "size": size}


def _source_file(identity: str, path: Path) -> dict[str, int | str]:
    return {"path": identity, **_file_record(path)}


def rootfs_identity(rootfs: Path) -> dict[str, int | str]:
    """Return the rootfs bytes consumed by Kbuild."""
    return _file_record(rootfs)


def rootfs_input_path(work: Path, rootfs: dict[str, int | str]) -> Path:
    """Return the fixed initramfs input path passed to Kbuild."""
    _rootfs_record(rootfs)
    return work / _ROOTFS_INPUT


def create_plan(  # noqa: PLR0913 -- exact causal inputs remain separate.
    *,
    linux_recipe: str,
    defconfig: Path,
    defconfig_path: str,
    rootfs: dict[str, int | str],
    rootfs_input: Path,
    buildroot_receipt: dict[str, str],
    arch: str,
    cross_compile: Path,
    commands: list[list[str]],
    outputs: tuple[str, ...],
    implementation: list[tuple[str, Path]],
    linux_base: str | None = None,
) -> KbuildPlan:
    """Create the exact recipe whose successful output may be reused.

    A changed recipe is deliberately only a cache miss.  The executor retains
    the fixed ``work/kernel`` directory and lets Kbuild reconcile it through
    its normal defconfig and ``olddefconfig`` steps.
    """
    prepared_linux_recipe = _require_digest(linux_recipe, "prepared Linux recipe")
    rootfs_record = _rootfs_record(rootfs)
    if not isinstance(buildroot_receipt, dict) or set(buildroot_receipt) != {"recipe", "sha256"}:
        _error("Buildroot receipt identity is invalid")
    buildroot_identity = {
        "recipe": _require_digest(buildroot_receipt.get("recipe"), "Buildroot recipe"),
        "sha256": _require_digest(buildroot_receipt.get("sha256"), "Buildroot receipt SHA-256"),
    }
    if not isinstance(arch, str) or not arch:
        _error("Kbuild ARCH must be a non-empty string")
    normalized_outputs = tuple(_relative(value, "Kbuild output") for value in outputs)
    if not normalized_outputs or len(set(normalized_outputs)) != len(normalized_outputs):
        _error("Kbuild outputs must be unique and non-empty")
    normalized_commands: list[list[str]] = []
    for command in commands:
        if not command or not all(isinstance(value, str) and value for value in command):
            _error("Kbuild command must contain non-empty strings")
        normalized_commands.append(_recipe_command(command))
    implementation_records = [
        _source_file(_relative(identity, "Kbuild implementation path"), path)
        for identity, path in implementation
    ]
    manifest: dict[str, object] = {
        "prepared_linux_recipe": prepared_linux_recipe,
        "defconfig": _source_file(_relative(defconfig_path, "Kbuild defconfig"), defconfig),
        "rootfs": rootfs_record,
        "rootfs_input": str(rootfs_input),
        "buildroot_receipt": buildroot_identity,
        "arch": arch,
        "cross_compile": str(cross_compile),
        "commands": normalized_commands,
        "outputs": list(normalized_outputs),
        "implementation": implementation_records,
    }
    if linux_base is not None:
        manifest["linux_base"] = _require_digest(linux_base, "Linux base source")
    recipe = hashlib.sha256(_canonical_json(manifest)).hexdigest()
    return KbuildPlan(
        recipe=recipe,
        rootfs=rootfs_record,
        rootfs_input=rootfs_input,
        outputs=normalized_outputs,
    )


def _receipt_path(work: Path) -> Path:
    return work / RECEIPT_NAME


def _receipt_payload(plan: KbuildPlan) -> dict[str, object]:
    return {"recipe": plan.recipe, "outputs": list(plan.outputs)}


def _outputs_exist(output: Path, plan: KbuildPlan) -> bool:
    if not output.is_dir():
        return False
    return all((output / relative).is_file() for relative in plan.outputs)


def _read_json(path: Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def _write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.stem}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(_canonical_json(value))
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def cache_hit(work: Path, output: Path, plan: KbuildPlan) -> bool:
    """Return true only when the exact success receipt and outputs exist."""
    receipt = _read_json(_receipt_path(work))
    return (
        receipt == _receipt_payload(plan)
        and _outputs_exist(output, plan)
        and _file_record_or_none(plan.rootfs_input) == plan.rootfs
    )


def discard_success_receipt(work: Path) -> None:
    """Remove a prior success receipt before changing Kbuild inputs or outputs."""
    _receipt_path(work).unlink(missing_ok=True)


def prepare_output(work: Path, output: Path) -> None:
    """Create the fixed Kbuild ``O=`` while retaining its incremental state."""
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    if output != work / "kernel":
        _error(f"refusing to prepare unmanaged Kbuild output: {output}")
    try:
        output.mkdir(mode=0o700, parents=True, exist_ok=True)
    except OSError as error:
        raise KbuildStateError(f"Kbuild output directory is invalid: {output}") from error
    if not output.is_dir():
        _error(f"Kbuild output directory is invalid: {output}")


def materialize_rootfs_input(work: Path, source: Path, plan: KbuildPlan) -> Path:
    """Copy the current rootfs to the digest-derived Kbuild input path."""
    destination = plan.rootfs_input
    if destination != rootfs_input_path(work, plan.rootfs):
        _error("Kbuild rootfs input path is outside the managed directory")
    if _file_record_or_none(destination) == plan.rootfs:
        return destination
    if rootfs_identity(source) != plan.rootfs:
        _error("Buildroot rootfs changed while preparing Kbuild input")
    descriptor, temporary_name = tempfile.mkstemp(
        dir=work,
        prefix=f".{destination.name}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as target, source.open("rb") as origin:
            shutil.copyfileobj(origin, target)
        if _file_record(temporary) != plan.rootfs:
            _error("materialized Kbuild rootfs differs from its digest")
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def publish_success(work: Path, output: Path, plan: KbuildPlan) -> None:
    """Publish the exact receipt only after every declared output exists."""
    if not _outputs_exist(output, plan):
        _error("Kbuild declared output is missing or invalid")
    if _file_record_or_none(plan.rootfs_input) != plan.rootfs:
        _error("Kbuild rootfs input is missing or differs from its digest")
    _write_json_atomic(_receipt_path(work), _receipt_payload(plan))


def receipt_identity(work: Path, output: Path, plan: KbuildPlan) -> dict[str, str]:
    """Return an identity only after rechecking the exact success receipt."""
    if not cache_hit(work, output, plan):
        _error("Kbuild receipt is missing, stale, or invalid")
    receipt = _receipt_path(work)
    return {"recipe": plan.recipe, "sha256": sha256_file(receipt)}

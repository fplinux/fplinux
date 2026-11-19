# SPDX-License-Identifier: GPL-2.0-only
"""Exact Kbuild receipt for one stable ``work/kernel`` output directory."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import NoReturn

from fplinux_cli.common import (
    canonical_json_bytes,
    is_sha256,
    read_json_object,
    replace_file_atomically,
    sha256_file,
)

RECEIPT_NAME = ".fplinux-kbuild-receipt.json"


class KbuildStateError(ValueError):
    """Raised when the Kbuild receipt cannot describe the current output."""


def _error(message: str) -> NoReturn:
    raise KbuildStateError(message)


@dataclass(frozen=True)
class KbuildPlan:
    """The exact recipe and files required to reuse ``work/kernel``."""

    recipe: str
    outputs: tuple[str, ...]


def _require_digest(value: object, field: str) -> str:
    if not is_sha256(value):
        _error(f"{field} must be a lowercase SHA-256 digest")
    return str(value)


def _relative(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        _error(f"{field} must be a non-empty relative path")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or value != path.as_posix():
        _error(f"{field} must be a normalized relative path: {value!r}")
    return value


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


def _source_file(identity: str, path: Path) -> dict[str, int | str]:
    return {"path": identity, **_file_record(path)}


def _implementation_records(
    implementation: list[tuple[str, Path]],
) -> list[dict[str, int | str]]:
    return [
        _source_file(_relative(identity, "Kbuild implementation path"), path)
        for identity, path in implementation
    ]


def implementation_identity(implementation: list[tuple[str, Path]]) -> str:
    """Identify the exact builder implementation that can change kernel bytes."""
    records = _implementation_records(implementation)
    if not records:
        _error("Kbuild implementation must not be empty")
    return hashlib.sha256(canonical_json_bytes(records)).hexdigest()


def create_plan(  # noqa: PLR0913 -- exact causal inputs remain separate.
    *,
    linux_recipe: str,
    defconfig: Path,
    defconfig_path: str,
    arch: str,
    cross_compile: str,
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
    if not isinstance(arch, str) or not arch:
        _error("Kbuild ARCH must be a non-empty string")
    normalized_outputs = tuple(_relative(value, "Kbuild output") for value in outputs)
    if not normalized_outputs or len(set(normalized_outputs)) != len(normalized_outputs):
        _error("Kbuild outputs must be unique and non-empty")
    normalized_commands: list[list[str]] = []
    for command in commands:
        if (
            not command
            or not isinstance(command[0], str)
            or not command[0]
            or not all(isinstance(value, str) for value in command)
        ):
            _error("Kbuild command must begin with a non-empty executable string")
        normalized_commands.append(_recipe_command(command))
    implementation_records = _implementation_records(implementation)
    manifest: dict[str, object] = {
        "prepared_linux_recipe": prepared_linux_recipe,
        "defconfig": _source_file(_relative(defconfig_path, "Kbuild defconfig"), defconfig),
        "arch": arch,
        "cross_compile": str(cross_compile),
        "commands": normalized_commands,
        "outputs": list(normalized_outputs),
        "implementation": implementation_records,
    }
    if linux_base is not None:
        manifest["linux_base"] = _require_digest(linux_base, "Linux base source")
    recipe = hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()
    return KbuildPlan(
        recipe=recipe,
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


def _write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    replace_file_atomically(path, canonical_json_bytes(value), 0o600, sync=False)


def cache_hit(work: Path, output: Path, plan: KbuildPlan) -> bool:
    """Return true only when the exact success receipt and outputs exist."""
    receipt = read_json_object(_receipt_path(work))
    return receipt == _receipt_payload(plan) and _outputs_exist(output, plan)


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


def publish_success(work: Path, output: Path, plan: KbuildPlan) -> None:
    """Publish the exact receipt only after every declared output exists."""
    if not _outputs_exist(output, plan):
        _error("Kbuild declared output is missing or invalid")
    _write_json_atomic(_receipt_path(work), _receipt_payload(plan))


def receipt_identity(work: Path, output: Path, plan: KbuildPlan) -> dict[str, str]:
    """Return an identity only after rechecking the exact success receipt."""
    if not cache_hit(work, output, plan):
        _error("Kbuild receipt is missing, stale, or invalid")
    receipt = _receipt_path(work)
    return {"recipe": plan.recipe, "sha256": sha256_file(receipt)}

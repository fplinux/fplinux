# SPDX-License-Identifier: GPL-2.0-only
"""Cache successful exact per-scope checks."""

from __future__ import annotations

import hashlib
import json
import sys
from dataclasses import dataclass
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail, replace_file_atomically
from fplinux_cli.environment.images import container_image_recipe_digest, file_recipe_digest
from fplinux_cli.manifests.values import TARGET_NAME

from .scopes import SOURCE_CHECK_SCOPES

if TYPE_CHECKING:
    from pathlib import Path

_SCOPE_ROOT = "check-results"


@dataclass(frozen=True)
class CheckReceiptRecipe:
    """Every input that permits a successful check result to be reused."""

    scope: str
    closure_digest: str
    orchestration_recipe: str
    image_generation: str
    profile: str | None = None
    build_type: str | None = None

    def payload(self) -> dict[str, object]:
        """Return the exact persisted payload for one successful check."""
        return {
            "result": "success",
            "scope": self.scope,
            "closure_digest": self.closure_digest,
            "orchestration_recipe": self.orchestration_recipe,
            "image_generation": self.image_generation,
            "profile": self.profile,
            "build_type": self.build_type,
        }


def check_closure_entries_digest(entries: list[tuple[str, bytes, int]]) -> str:
    """Hash one captured check closure."""
    value = hashlib.sha256()
    value.update(b"fplinux.check-closure\0")
    for relative, contents, mode in sorted(entries):
        for field in (relative.encode(), mode.to_bytes(2, "big"), contents):
            value.update(len(field).to_bytes(8, "big"))
            value.update(field)
    return value.hexdigest()


def receipt_path(cache_root: Path, recipe: CheckReceiptRecipe) -> Path:
    """Return the one default or named-profile receipt path for a scope."""
    root = cache_root / _SCOPE_ROOT
    if recipe.profile is not None:
        if TARGET_NAME.fullmatch(recipe.profile) is None:
            raise ValueError(f"invalid receipt profile: {recipe.profile!r}")
        root = root / "profiles" / recipe.profile
    if recipe.build_type is not None:
        if recipe.build_type not in ("release", "debug"):
            raise ValueError(f"invalid receipt build type: {recipe.build_type!r}")
        root = root / "builds" / recipe.build_type
    return root / recipe.scope / "success.json"


def receipt_matches(cache_root: Path, recipe: CheckReceiptRecipe) -> bool:
    """Return whether an exact successful receipt exists."""
    return _read_payload(receipt_path(cache_root, recipe)) == recipe.payload()


def _read_payload(path: Path) -> object | None:
    """Read a receipt; unreadable or invalid JSON is a cache miss."""
    if path.is_symlink():
        return None
    try:
        with path.open(encoding="utf-8") as stream:
            value: object = json.load(stream)
            return value
    except OSError, UnicodeDecodeError, json.JSONDecodeError:
        return None


def publish_success_receipt(cache_root: Path, recipe: CheckReceiptRecipe) -> None:
    """Atomically publish a receipt after a scope has completed successfully."""
    destination = receipt_path(cache_root, recipe)
    destination.parent.mkdir(parents=True, exist_ok=True)
    replace_file_atomically(destination, canonical_json_bytes(recipe.payload()), 0o600)


def check_orchestration_recipe_digest(image_recipe: str | None = None) -> str:
    """Hash only the implementation that can change cached check results."""
    fixed = [
        common.ROOT / "scripts/fplinux_cli/__init__.py",
        common.ROOT / "scripts/fplinux_cli/common.py",
        common.ROOT / "scripts/fplinux_cli/alpine/__init__.py",
        common.ROOT / "scripts/fplinux_cli/alpine/registration.py",
        common.ROOT / "scripts/fplinux_cli/environment/__init__.py",
        common.ROOT / "scripts/fplinux_cli/environment/downloads.py",
        common.ROOT / "scripts/fplinux_cli/environment/images.py",
        common.ROOT / "scripts/fplinux_cli/environment/image_state.py",
        common.ROOT / "scripts/fplinux_cli/environment/image_store.py",
        common.ROOT / "scripts/fplinux_cli/environment/kern.py",
        common.ROOT / "scripts/fplinux_cli/environment/setup.py",
        common.ROOT / "scripts/fplinux_cli/environment/git_hooks.py",
        common.ROOT / "scripts/fplinux_cli/quality/__init__.py",
        common.ROOT / "scripts/fplinux_cli/quality/checks.py",
        common.ROOT / "scripts/fplinux_cli/quality/scopes.py",
        common.ROOT / "scripts/fplinux_cli/quality/inputs.py",
        common.ROOT / "scripts/fplinux_cli/quality/receipts.py",
        common.ROOT / "scripts/fplinux_cli/quality/runtime.py",
        common.ROOT / "scripts/fplinux_cli/quality/testing.py",
        common.ROOT / "scripts/fplinux_cli/quality/host_testing.py",
        common.ROOT / "scripts/fplinux_cli/dependencies/__init__.py",
        common.ROOT / "scripts/fplinux_cli/dependencies/inputs.py",
        common.ROOT / "scripts/fplinux_cli/quality/git.py",
        common.ROOT / "scripts/fplinux_cli/quality/source_gate.py",
        common.ROOT / "scripts/fplinux_cli/workspace/__init__.py",
        common.ROOT / "scripts/fplinux_cli/workspace/capture.py",
        common.ROOT / "scripts/fplinux_cli/workspace/quality_inputs.py",
        common.ROOT / "scripts/fplinux_cli/workspace/staging.py",
        common.ROOT / "scripts/fplinux_cli/manifests/__init__.py",
        common.ROOT / "scripts/fplinux_cli/manifests/assets.py",
        common.ROOT / "scripts/fplinux_cli/manifests/identity.py",
        common.ROOT / "scripts/fplinux_cli/manifests/kernel.py",
        common.ROOT / "scripts/fplinux_cli/manifests/linux.py",
        common.ROOT / "scripts/fplinux_cli/manifests/paths.py",
        common.ROOT / "scripts/fplinux_cli/manifests/platforms.py",
        common.ROOT / "scripts/fplinux_cli/manifests/profiles.py",
        common.ROOT / "scripts/fplinux_cli/manifests/releases.py",
        common.ROOT / "scripts/fplinux_cli/manifests/targets.py",
        common.ROOT / "scripts/fplinux_cli/manifests/values.py",
        common.ROOT / "scripts/fplinux_cli/reporting/__init__.py",
        common.ROOT / "scripts/fplinux_cli/reporting/run.py",
        common.ROOT / "scripts/fplinux_cli/reporting/process.py",
    ]
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    return file_recipe_digest(fixed, prefix=bytes.fromhex(image_recipe))


def check_scope_receipt_recipe(  # noqa: PLR0913 -- source, image and kernel type stay explicit.
    scope: str,
    closure_digest: str,
    *,
    image_generation: str,
    orchestration_recipe: str | None = None,
    profile: str | None = None,
    build_type: str = "release",
) -> CheckReceiptRecipe:
    """Bind one cacheable source scope to its exact closure and OCI identities."""
    if scope not in (*SOURCE_CHECK_SCOPES, "kernel"):
        fail(f"check scope does not support receipts: {scope}")
    if orchestration_recipe is None:
        orchestration_recipe = check_orchestration_recipe_digest()
    if scope == "python":
        orchestration_recipe = hashlib.sha256(
            canonical_json_bytes(
                {
                    "check_implementation": orchestration_recipe,
                    "host_python": sys.version,
                    "host_cache_tag": sys.implementation.cache_tag,
                }
            )
        ).hexdigest()
    return CheckReceiptRecipe(
        scope=scope,
        closure_digest=closure_digest,
        orchestration_recipe=orchestration_recipe,
        image_generation=image_generation,
        profile=profile,
        build_type=build_type if scope == "kernel" else None,
    )

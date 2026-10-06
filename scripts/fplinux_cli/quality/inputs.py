# SPDX-License-Identifier: GPL-2.0-only
"""Select the captured inputs that can affect each quality scope."""

from __future__ import annotations

import re
import tomllib
from pathlib import Path, PurePath
from typing import TYPE_CHECKING

from fplinux_cli.alpine import registration
from fplinux_cli.common import fail
from fplinux_cli.manifests.paths import normalize_profile
from fplinux_cli.quality.formatting.source_formats import (
    is_posix_shell_fragment,
    shell_dialect,
    source_format_kind,
)

from .receipts import check_closure_entries_digest
from .scopes import SOURCE_CHECK_SCOPES

if TYPE_CHECKING:
    from fplinux_cli.workspace.capture import WorkspaceFile, WorkspaceSnapshot


_QUOTED_C_INCLUDE = re.compile(rb'^\s*#\s*include\s*"([^"\n]+)"', re.MULTILINE)


_PRETTIER_CONFIGURATION_NAMES = frozenset(
    {
        ".prettierrc",
        ".prettierrc.cjs",
        ".prettierrc.cts",
        ".prettierrc.js",
        ".prettierrc.json",
        ".prettierrc.json5",
        ".prettierrc.mjs",
        ".prettierrc.mts",
        ".prettierrc.toml",
        ".prettierrc.ts",
        ".prettierrc.yaml",
        ".prettierrc.yml",
        "prettier.config.cjs",
        "prettier.config.cts",
        "prettier.config.js",
        "prettier.config.mjs",
        "prettier.config.mts",
        "prettier.config.ts",
    }
)


_EXECUTABLE_PRETTIER_CONFIGURATION_NAMES = frozenset(
    name
    for name in _PRETTIER_CONFIGURATION_NAMES
    if Path(name).suffix in {".cjs", ".cts", ".js", ".mjs", ".mts", ".ts"}
)


_MANIFEST_IMPLEMENTATION = frozenset(
    {
        "scripts/fplinux_cli/manifests/__init__.py",
        "scripts/fplinux_cli/manifests/assets.py",
        "scripts/fplinux_cli/manifests/identity.py",
        "scripts/fplinux_cli/manifests/kernel.py",
        "scripts/fplinux_cli/manifests/linux.py",
        "scripts/fplinux_cli/manifests/paths.py",
        "scripts/fplinux_cli/manifests/platforms.py",
        "scripts/fplinux_cli/manifests/profiles.py",
        "scripts/fplinux_cli/manifests/releases.py",
        "scripts/fplinux_cli/manifests/targets.py",
        "scripts/fplinux_cli/manifests/values.py",
    }
)


_CHECK_IMPLEMENTATION = _MANIFEST_IMPLEMENTATION | frozenset(
    {
        "scripts/check.py",
        "scripts/site_collect.py",
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/alpine/__init__.py",
        "scripts/fplinux_cli/alpine/registration.py",
        "scripts/fplinux_cli/alpine/lock.py",
        "scripts/fplinux_cli/alpine/selection.py",
        "scripts/fplinux_cli/workspace/__init__.py",
        "scripts/fplinux_cli/workspace/capture.py",
        "scripts/fplinux_cli/environment/__init__.py",
        "scripts/fplinux_cli/environment/images.py",
        "scripts/fplinux_cli/quality/__init__.py",
        "scripts/fplinux_cli/quality/source_gate.py",
        "scripts/fplinux_cli/quality/testing.py",
        "scripts/fplinux_cli/quality/runtime.py",
        "scripts/fplinux_cli/reporting/__init__.py",
        "scripts/fplinux_cli/reporting/run.py",
        "scripts/fplinux_cli/reporting/process.py",
        "scripts/fplinux_cli/quality/formatting/__init__.py",
        "scripts/fplinux_cli/quality/formatting/canonical.py",
        "scripts/fplinux_cli/quality/formatting/canonical_json.py",
        "scripts/fplinux_cli/quality/formatting/canonical_json_tree.mjs",
        "scripts/fplinux_cli/quality/formatting/canonical_markdown.py",
        "scripts/fplinux_cli/quality/formatting/canonical_text.py",
        "scripts/fplinux_cli/quality/formatting/canonical_toml.py",
        "scripts/fplinux_cli/quality/formatting/canonical_yaml.py",
        "scripts/fplinux_cli/quality/formatting/source_formats.py",
    }
)


_KERNEL_IMPLEMENTATION = _MANIFEST_IMPLEMENTATION | frozenset(
    {
        "scripts/fplinux_cli/__init__.py",
        "scripts/fplinux_cli/common.py",
        "scripts/fplinux_cli/build/__init__.py",
        "scripts/fplinux_cli/build/environment.py",
        "scripts/fplinux_cli/build/device_tree.py",
        "scripts/fplinux_cli/build/identity.py",
        "scripts/fplinux_cli/build/inputs.py",
        "scripts/fplinux_cli/build/process.py",
        "scripts/fplinux_cli/build/sources.py",
        "scripts/fplinux_cli/build/kernel/__init__.py",
        "scripts/fplinux_cli/build/kernel/configuration.py",
        "scripts/fplinux_cli/build/kernel/prepare.py",
        "scripts/fplinux_cli/build/kernel/projection.py",
        "scripts/fplinux_cli/build/kernel/state.py",
        "scripts/fplinux_cli/build/storage/__init__.py",
        "scripts/fplinux_cli/build/storage/layout.py",
        "scripts/fplinux_cli/workspace/__init__.py",
        "scripts/fplinux_cli/workspace/capture.py",
        "scripts/fplinux_cli/quality/__init__.py",
        "scripts/fplinux_cli/quality/kernel_patches.py",
        "scripts/fplinux_cli/quality/kernel/__init__.py",
        "scripts/fplinux_cli/quality/kernel/__main__.py",
        "scripts/fplinux_cli/quality/kernel/contexts.py",
        "scripts/fplinux_cli/quality/kernel/analysis.py",
        "scripts/fplinux_cli/quality/kernel/runtime.py",
        "scripts/fplinux_cli/quality/kernel/workers.py",
        "scripts/fplinux_cli/reporting/__init__.py",
        "scripts/fplinux_cli/reporting/run.py",
        "scripts/fplinux_cli/reporting/process.py",
    }
)


def _is_shell_source(file: WorkspaceFile) -> bool:
    if is_posix_shell_fragment(file.path):
        return True
    if Path(file.path).suffix not in {"", ".initd", ".sh", ".bashrc"}:
        return False
    first_line = file.contents.splitlines()[:1]
    if not first_line:
        return False
    return shell_dialect(first_line[0]) is not None


def _is_prettier_configuration(path: str) -> bool:
    name = Path(path).name
    return (
        name in {".gitignore", ".prettierignore", "package.yaml"}
        or name in _PRETTIER_CONFIGURATION_NAMES
    )


def _source_scope_uses_file(  # noqa: PLR0911
    scope: str, file: WorkspaceFile
) -> bool:
    """Return whether one captured file can affect the selected source scope."""
    path = PurePath(file.path)
    name = path.name
    suffix = path.suffix
    parts = path.parts
    if file.path in _CHECK_IMPLEMENTATION:
        return True
    if scope in {
        "source",
        "docs",
        "spelling",
        "secrets",
        "licenses",
        "python",
    }:
        return True
    if scope == "container":
        return name in {".kernignore", "Containerfile"} or name.startswith(".hadolint")
    if scope == "metadata":
        return (
            source_format_kind(file.path) in {"toml", "json", "yaml", "javascript"}
            or suffix == ".ini"
            or name == ".editorconfig"
            or _is_prettier_configuration(file.path)
        )
    if scope == "shell":
        return _is_shell_source(file) or name in {".editorconfig", ".shellcheckrc"}
    if scope == "alpine":
        return (
            file.path in {"alpine.lock.toml", "alpine/abuild.conf"}
            or (len(parts) == 3 and parts[0] == "targets" and name == "target.toml")
            or (len(parts) == 3 and parts[0] == "platforms" and name == "platform.toml")
            or (len(parts) == 4 and parts[:2] == ("alpine", "aports") and name == "APKBUILD")
        )
    if scope == "c":
        return (
            name in {".clang-format", ".clang-format-ignore", "_clang-format"}
            or (len(parts) == 3 and parts[0] == "targets" and name == "target.toml")
            or (len(parts) == 3 and parts[0] == "platforms" and name == "platform.toml")
        )
    fail(f"check scope does not support a source closure: {scope}")
    return False


def _c_scope_paths(snapshot: WorkspaceSnapshot) -> set[str]:
    """Resolve the same userspace/bootstrap C inputs and their quoted headers."""
    by_path = {file.path: file for file in snapshot.files}
    selected = {file.path for file in snapshot.files if _source_scope_uses_file("c", file)}
    for file in snapshot.files:
        path = PurePath(file.path)
        if path.suffix in {".c", ".h"} and (
            path.parts[:2] == ("alpine", "aports")
            or file.path in registration.SHARED_APORT_SOURCE_PATHS
            or path.parts[0] == "tests"
            or (
                len(path.parts) >= 4
                and path.parts[0] in {"platforms", "targets"}
                and path.parts[2] in {"common", "uboot"}
            )
        ):
            selected.add(file.path)
        if path.suffix in {".c", ".h"} and "bootstrap" in path.parts:
            selected.add(file.path)

    for file in snapshot.files:
        path = PurePath(file.path)
        if len(path.parts) != 3 or path.parts[0] != "platforms" or path.name != "platform.toml":
            continue
        try:
            manifest = tomllib.loads(file.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        tools = manifest.get("host", {}).get("tools", [])
        if not isinstance(tools, list):
            continue
        for recipe in tools:
            if isinstance(recipe, dict) and recipe.get("type") == "cc-libusb":
                source = recipe.get("source")
                if isinstance(source, str) and source in by_path:
                    selected.add(source)

    pending = list(selected)
    while pending:
        relative = pending.pop()
        source = by_path.get(relative)
        if source is None or PurePath(relative).suffix not in {".c", ".h"}:
            continue
        for raw_include in _QUOTED_C_INCLUDE.findall(source.contents):
            try:
                include = raw_include.decode("utf-8")
            except UnicodeDecodeError:
                continue
            candidates = (
                (PurePath(relative).parent / include).as_posix(),
                PurePath(include).as_posix(),
            )
            for candidate in candidates:
                if candidate in by_path and candidate not in selected:
                    selected.add(candidate)
                    pending.append(candidate)
                    break
    return selected


def _linux_manifest_sources(linux: object, *, base: PurePath) -> set[str]:
    """Return the captured Linux inputs explicitly named by one manifest."""
    if not isinstance(linux, dict):
        return set()
    selected: set[str] = set()
    for key in ("defconfig", "config_fragment"):
        value = linux.get(key)
        if isinstance(value, str):
            selected.add((base / value).as_posix())
    patches = linux.get("patches")
    if isinstance(patches, list):
        selected.update((base / patch).as_posix() for patch in patches if isinstance(patch, str))
    for key in ("copies", "appends"):
        steps = linux.get(key)
        if isinstance(steps, list):
            selected.update(
                (base / source).as_posix()
                for step in steps
                if isinstance(step, dict) and isinstance((source := step.get("source")), str)
            )
    return selected


def _kernel_scope_paths(
    snapshot: WorkspaceSnapshot, profile: str | None = None, *, build_type: str = "release"
) -> set[str]:
    """Resolve the selected global profile and every board's Linux inputs."""
    profile = normalize_profile(profile)
    by_path = {file.path: file for file in snapshot.files}
    selected = {
        file.path
        for file in snapshot.files
        if file.path in _KERNEL_IMPLEMENTATION or file.path == "sources.lock.toml"
    }
    selected.add(f"profiles/{profile or 'default'}/profile.toml")
    target_manifests = [
        file
        for file in snapshot.files
        if (path := PurePath(file.path)).parts[:1] == ("targets",)
        and len(path.parts) == 3
        and path.name == "target.toml"
    ]
    for target_manifest in target_manifests:
        target_path = PurePath(target_manifest.path)
        selected.add(target_manifest.path)
        try:
            target_data = tomllib.loads(target_manifest.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        selected.update(_linux_manifest_sources(target_data.get("linux"), base=target_path.parent))
        if profile == "microsd-uboot":
            microsd = target_data.get("microsd", {})
            if isinstance(microsd, dict):
                selected.update(
                    _linux_manifest_sources(
                        {"patches": microsd.get("linux_patches")}, base=target_path.parent
                    )
                )
        platform = target_data.get("platform")
        if not isinstance(platform, str):
            continue
        platform_path = PurePath("platforms") / platform / "platform.toml"
        platform_manifest = by_path.get(platform_path.as_posix())
        if platform_manifest is None:
            continue
        selected.add(platform_manifest.path)
        try:
            platform_data = tomllib.loads(platform_manifest.contents.decode("utf-8"))
        except UnicodeDecodeError, tomllib.TOMLDecodeError:
            continue
        linux = platform_data.get("linux")
        selected.update(_linux_manifest_sources(linux, base=PurePath()))
        if isinstance(linux, dict) and isinstance(linux.get("build_types"), dict):
            fragment = linux["build_types"].get(build_type)
            if isinstance(fragment, str):
                selected.add(fragment)
    return selected


def check_scope_closure_digest(
    scope: str,
    snapshot: WorkspaceSnapshot,
    *,
    profile: str | None = None,
    build_type: str = "release",
) -> str:
    """Hash only captured files that can affect one exact check scope."""
    if scope == "kernel":
        paths = _kernel_scope_paths(snapshot, profile, build_type=build_type)
        selected = [file for file in snapshot.files if file.path in paths]
    elif scope == "c":
        paths = _c_scope_paths(snapshot)
        selected = [file for file in snapshot.files if file.path in paths]
    elif scope in SOURCE_CHECK_SCOPES:
        broaden = (
            scope == "metadata"
            and any(
                Path(file.path).name in _EXECUTABLE_PRETTIER_CONFIGURATION_NAMES
                for file in snapshot.files
            )
        ) or (
            scope == "shell"
            and any(
                file.path == ".shellcheckrc" and b"external-sources=true" in file.contents
                for file in snapshot.files
            )
        )
        selected = [
            file for file in snapshot.files if broaden or _source_scope_uses_file(scope, file)
        ]
    else:
        fail(f"check scope does not support receipts: {scope}")
    if not selected:
        fail(f"check scope has an empty causal closure: {scope}")
    return check_closure_entries_digest(
        [(file.path, file.contents, file.mode) for file in selected]
    )

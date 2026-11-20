# SPDX-License-Identifier: GPL-2.0-only
"""Capture the common public source identity before selecting a target."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.artifacts.release_identity import (
    RELEASE_FIELDS,
    ReleaseContext,
    release_context_from_fields,
    require_release_version,
)
from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes
from fplinux_cli.data_paths import DATA_DIR_NAME, PRIVATE_DIR_NAME, data_root
from fplinux_cli.source_paths import git_source_excludes, source_path_excluded

if TYPE_CHECKING:
    from fplinux_cli.workspace.capture import WorkspaceFile

RELEASE_VERSION_NAME = "VERSION"
SOURCE_IDENTITY_NAME = ".fplinux-release.json"
BUILD_IDENTITY_NAME = ".fplinux-build-release.json"
_SOURCE_DIRECTORIES = frozenset(
    {
        ".githooks",
        ".github",
        "LICENSES",
        "alpine",
        "bootstrap",
        "common",
        "include",
        "lib",
        "platforms",
        "profiles",
        "scripts",
        "site",
        "targets",
        "tests",
        "third-party-licenses",
    }
)
_ROOT_FILES = frozenset(
    {
        ".clang-format",
        ".editorconfig",
        ".gitattributes",
        ".gitignore",
        ".markdownlint-cli2.jsonc",
        ".prettierignore",
        ".prettierrc.json",
        ".taplo.toml",
        ".vale.ini",
        "LICENSE",
        "README.md",
        "REUSE.toml",
        "THIRD_PARTY_NOTICES.md",
        RELEASE_VERSION_NAME,
        "_typos.toml",
        "commitlint.config.mjs",
        "fplinux",
        "package-lock.json",
        "package.json",
        "pyproject.toml",
    }
)
_EXCLUDED_COMPONENTS = frozenset(
    {
        ".astro",
        ".cache",
        DATA_DIR_NAME,
        PRIVATE_DIR_NAME,
        ".git",
        ".pytest_cache",
        ".ruff_cache",
        ".venv",
        "__pycache__",
        "diagnostics",
        "dist",
        "dumps",
        "evidence",
        "handoffs",
        "keys",
        "node_modules",
        "private",
        "research",
        "signing",
    }
)


@dataclass(frozen=True)
class PublicSourceSnapshot:
    """Exact corresponding source bytes and modes, captured before any build."""

    files: tuple[WorkspaceFile, ...]
    release: ReleaseContext


def _public_source(relative: str) -> bool:
    path = PurePosixPath(relative)
    if not path.parts or path.is_absolute() or ".." in path.parts or path.as_posix() != relative:
        fail(f"public source path is unsafe: {relative}")
    if any(part in _EXCLUDED_COMPONENTS or part == "AGENTS.md" for part in path.parts):
        return False
    if (
        path.name.startswith(".env")
        or path.suffix.lower()
        in {
            ".apk",
            ".bin",
            ".cpio",
            ".dll",
            ".dump",
            ".elf",
            ".exe",
            ".key",
            ".pem",
            ".pub",
            ".pyc",
            ".pyo",
        }
        or path.name.endswith("~")
    ):
        return False
    if len(path.parts) == 1:
        return relative in _ROOT_FILES or relative.endswith(".lock.toml")
    return path.parts[0] in _SOURCE_DIRECTORIES


def _git(root: Path, *arguments: str) -> bytes:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), *arguments], capture_output=True, check=False, timeout=60
        )
    except subprocess.TimeoutExpired:
        fail("public source Git inventory timed out after 60s")
    if result.returncode:
        detail = result.stderr.decode(errors="replace").strip()
        fail(f"cannot read public source Git identity: {detail}")
    return result.stdout


def _source_paths(root: Path, *, git_checkout: bool, selected_data_root: Path) -> tuple[str, ...]:
    if git_checkout:
        names = _git(
            root,
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            *git_source_excludes(source_root=root, selected_data_root=selected_data_root),
        )
        paths = {os.fsdecode(name) for name in names.split(b"\0") if name}
    else:
        paths = set()
        for directory, children, files in os.walk(root, followlinks=False):
            base = Path(directory).relative_to(root)
            children[:] = [
                child
                for child in children
                if child not in _EXCLUDED_COMPONENTS
                and (base != Path() or child in _SOURCE_DIRECTORIES)
                and not source_path_excluded(
                    root / base / child, source_root=root, selected_data_root=selected_data_root
                )
            ]
            for filename in files:
                paths.add((base / filename).as_posix())
    return tuple(
        relative
        for relative in sorted(paths)
        if _public_source(relative)
        and not source_path_excluded(
            root / relative, source_root=root, selected_data_root=selected_data_root
        )
        and (root / relative).exists()
    )


def _clean_commit(
    root: Path, files: tuple[WorkspaceFile, ...], *, selected_data_root: Path
) -> tuple[str, bool]:
    commit = _git(root, "rev-parse", "--verify", "HEAD").decode().strip()
    algorithm = _git(root, "rev-parse", "--show-object-format").decode().strip()
    tree = {}
    for entry in _git(root, "ls-tree", "-r", "-z", "HEAD").split(b"\0"):
        if not entry:
            continue
        properties, name = entry.split(b"\t", 1)
        mode, kind, object_hash = properties.split()
        relative = os.fsdecode(name)
        if _public_source(relative) and not source_path_excluded(
            root / relative, source_root=root, selected_data_root=selected_data_root
        ):
            tree[relative] = (int(mode, 8) & 0o777, kind, object_hash.decode())
    if set(tree) != {source.path for source in files}:
        return commit, False
    for source in files:
        blob = f"blob {len(source.contents)}\0".encode() + source.contents
        digest = hashlib.new(algorithm, blob, usedforsecurity=False).hexdigest()
        if tree[source.path] != (source.mode, b"blob", digest):
            return commit, False
    return commit, True


def _read_identity(path: Path) -> ReleaseContext:
    try:
        fields = json.loads(path.read_bytes())
    except (OSError, ValueError) as error:
        fail(f"release identity cannot be read: {path}: {error}")
    if not isinstance(fields, dict) or set(fields) != RELEASE_FIELDS:
        fail(f"release identity has unsupported fields: {path}")
    return release_context_from_fields(fields)


def public_source_digest(files: tuple[WorkspaceFile, ...]) -> str:
    """Hash the canonical public inventory supplied by a checkout or source archive."""
    if len({source.path for source in files}) != len(files):
        fail("public source inventory contains duplicate paths")
    for source in files:
        if not _public_source(source.path):
            fail(f"unexpected public source inventory member: {source.path}")
        if source.mode not in {0o644, 0o755}:
            fail(f"public source mode must be 0644 or 0755: {source.path}")
    records = [
        {"path": source.path, "sha256": sha256_bytes(source.contents), "mode": source.mode}
        for source in sorted(files, key=lambda source: source.path)
    ]
    return sha256_bytes(canonical_json_bytes(records))


def source_release_version(files: tuple[WorkspaceFile, ...]) -> str:
    """Read the release declaration from the same bytes used for the source digest."""
    for source in files:
        if source.path == RELEASE_VERSION_NAME:
            try:
                value = source.contents.decode("ascii").removesuffix("\n")
            except UnicodeDecodeError:
                fail(f"{RELEASE_VERSION_NAME} must contain an ASCII release version")
            return require_release_version(value)
    return fail(f"public sources lack the release version declaration: {RELEASE_VERSION_NAME}")


def capture_public_sources(
    root: Path | None = None, *, selected_data_root: Path | None = None
) -> PublicSourceSnapshot:
    """Capture one canonical inventory for both source export and the release ID."""
    from fplinux_cli.workspace.capture import (  # noqa: PLC0415 -- capture is not a metadata reader dependency.
        WorkspaceFile,
        read_source_file,
    )

    root = common.ROOT if root is None else root
    selected_data_root = data_root(root) if selected_data_root is None else selected_data_root
    git_checkout = (root / ".git").exists()
    captured = (
        read_source_file(relative, root / relative)
        for relative in _source_paths(
            root, git_checkout=git_checkout, selected_data_root=selected_data_root
        )
    )
    files = tuple(
        WorkspaceFile(source.path, source.contents, 0o755 if source.mode & 0o111 else 0o644)
        for source in captured
    )
    digest = public_source_digest(files)
    version = source_release_version(files)
    if git_checkout:
        commit, clean = _clean_commit(root, files, selected_data_root=selected_data_root)
        release = ReleaseContext(commit[:12] if clean else digest[:12], version, commit, digest)
    else:
        retained_path = root / SOURCE_IDENTITY_NAME
        retained = _read_identity(retained_path) if retained_path.is_file() else None
        if retained is not None and retained.source_snapshot == digest:
            if retained.release_version != version:
                fail(f"release identity does not match the declaration in {RELEASE_VERSION_NAME}")
            release = retained
        else:
            release = ReleaseContext(
                digest[:12], version, retained.source_commit if retained else None, digest
            )
    return PublicSourceSnapshot(files, release)


def current_release_context(
    root: Path | None = None, *, selected_data_root: Path | None = None
) -> ReleaseContext:
    """Resolve a source export or the context already bound to a staged build."""
    root = common.ROOT if root is None else root
    staged = root / BUILD_IDENTITY_NAME
    if (root / ".fplinux-workspace").is_file() and staged.is_file():
        return _read_identity(staged)
    return capture_public_sources(root, selected_data_root=selected_data_root).release


def source_identity_bytes(snapshot: PublicSourceSnapshot) -> bytes:
    """Deliver identity separately so it never hashes its own generated bytes."""
    return canonical_json_bytes(snapshot.release.fields())

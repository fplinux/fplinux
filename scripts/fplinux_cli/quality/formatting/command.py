# SPDX-License-Identifier: GPL-2.0-only
"""Format explicit project sources with the pinned quality tools."""

from __future__ import annotations

import os
import stat
import subprocess
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.build.kernel.state import inspect_linux_base
from fplinux_cli.common import ROOT, fail, relative_name, replace_file_atomically
from fplinux_cli.data_paths import data_root
from fplinux_cli.environment.kern import (
    kern_box_name,
    kern_environment,
)
from fplinux_cli.environment.runtime import environment_arguments, require_environment
from fplinux_cli.manifests.source_catalog import resolve_source_record
from fplinux_cli.quality.kernel_patches import linux_contexts
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace.capture import WorkspaceSnapshot, workspace_snapshot
from fplinux_cli.workspace.quality_inputs import quality_files, quality_workspace_snapshot

from .canonical import canonical_outputs, formatter_commands
from .source_formats import SourceFormats, classify_source_formats

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

_FORMAT_TIMEOUT_SECONDS = 15 * 60


@dataclass(frozen=True)
class FormatDecision:
    """One selected path and the tool that owns its formatting."""

    path: str
    kind: str
    reason: str


def _site_format_path(relative: str, root: Path) -> bool:
    path = PurePosixPath(relative)
    if path.parts[:1] != ("site",) or path.suffix not in {
        ".astro",
        ".css",
        ".js",
        ".json",
        ".jsonc",
        ".md",
        ".mdx",
        ".mjs",
        ".ts",
        ".yaml",
        ".yml",
    }:
        return False
    name = path.relative_to("site").as_posix()
    ignore = root / "site/.prettierignore"
    for raw in ignore.read_text().splitlines():
        rule = raw.strip().rstrip("/")
        if rule and not rule.startswith("#") and (name == rule or name.startswith(rule + "/")):
            return False
    return True


def plan_format_paths(values: Sequence[str]) -> list[FormatDecision]:
    """Classify all requested paths without starting a formatter or writing data."""
    files = quality_files(enforce_source_policy=False)
    by_path = dict(files)
    supported = classify_source_formats([path for _, path in files], root=ROOT).supported()
    decisions = []
    seen: set[str] = set()
    for value in values:
        try:
            relative = relative_name(value, field="format path")
        except SystemExit as error:
            decisions.append(FormatDecision(value, "invalid", str(error)))
            continue
        if relative in seen:
            decisions.append(FormatDecision(relative, "invalid", "duplicate path"))
            continue
        seen.add(relative)
        if _path_uses_symlink(ROOT, relative):
            decisions.append(
                FormatDecision(relative, "invalid", "format path must not use a symlink")
            )
        elif relative not in by_path:
            reason = (
                "format path must name a regular file"
                if (ROOT / relative).is_dir()
                else "format path is not a project source file"
            )
            decisions.append(FormatDecision(relative, "invalid", reason))
        elif relative in supported:
            decisions.append(FormatDecision(relative, "format", "project formatter"))
        elif relative.endswith(".patch"):
            try:
                linux_contexts(frozenset({relative}))
            except SystemExit as error:
                if "not a declared Linux patch:" not in str(error):
                    raise
                decisions.append(
                    FormatDecision(relative, "unsupported", "not a declared Linux patch")
                )
            else:
                decisions.append(FormatDecision(relative, "format", "declared Linux patch"))
        elif _site_format_path(relative, ROOT):
            decisions.append(
                FormatDecision(relative, "site", "use ./fplinux site format (whole site)")
            )
        else:
            decisions.append(
                FormatDecision(relative, "unsupported", "no project formatter is defined for")
            )
    return decisions


def _changed_format_paths() -> list[str]:
    paths: set[str] = set()
    for arguments in (
        ("diff", "--name-only", "--diff-filter=ACMR", "-z"),
        ("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"),
        ("ls-files", "--others", "--exclude-standard", "-z"),
    ):
        result = subprocess.run(
            ["git", *arguments, "--"], cwd=ROOT, capture_output=True, check=False, timeout=30
        )
        if result.returncode:
            fail(f"cannot select changed files: {result.stderr.decode(errors='replace').strip()}")
        paths.update(os.fsdecode(path) for path in result.stdout.split(b"\0") if path)
    return sorted(paths)


def format_command(values: Sequence[str], *, changed: bool, plan: bool) -> None:
    """Plan explicit or changed paths and format only the selected project sources."""
    if not values and not changed:
        fail("format requires PATH or --changed")
    selected = list(values)
    if changed:
        filters = [relative_name(value, field="format path") for value in values]
        if any(_path_uses_symlink(ROOT, value) for value in filters):
            fail("format path must not use a symlink")
        selected = [
            path
            for path in _changed_format_paths()
            if not filters
            or "." in filters
            or any(path == value or path.startswith(value + "/") for value in filters)
        ]
    decisions = plan_format_paths(selected)
    for item in decisions:
        if plan or (changed and item.kind != "format"):
            print(f"{item.kind}: {item.path}: {item.reason}")
    if plan:
        return
    problems = [item for item in decisions if item.kind != "format"]
    if problems and not changed:
        details = "\n".join(f"  {item.reason}: {item.path}" for item in problems)
        fail("format selection includes unsupported or invalid paths:\n" + details)
    paths = [item.path for item in decisions if item.kind == "format"]
    if not paths:
        print("format: no selected project sources")
        return
    format_sources(paths)


def _path_uses_symlink(root: Path, relative: str) -> bool:
    current = root
    for part in PurePosixPath(relative).parts:
        current /= part
        try:
            if current.is_symlink():
                return True
        except OSError:
            return True
    return False


def resolve_format_paths(
    values: Sequence[str],
    *,
    root: Path = ROOT,
    inventory: Sequence[tuple[str, Path]] | None = None,
) -> tuple[tuple[str, ...], list[tuple[str, Path]], SourceFormats]:
    """Resolve explicit formatter-owned paths from the Git quality inventory."""
    if inventory is None:
        inventory = quality_files(enforce_source_policy=False)
    files = list(inventory)
    by_path = dict(files)
    requested: list[str] = []
    seen: set[str] = set()

    for value in values:
        relative = relative_name(value, field="format path")
        if relative in seen:
            fail(f"format path is duplicated: {relative}")
        seen.add(relative)
        candidate = root / relative
        if _path_uses_symlink(root, relative):
            fail(f"format path must not use a symlink: {relative}")
        if relative not in by_path:
            if candidate.is_dir():
                fail(f"format path must name a regular file: {relative}")
            fail(f"format path is not a project source file: {relative}")
        requested.append(relative)

    formats = classify_source_formats([path for _relative, path in files], root=root)
    supported = formats.supported()
    for relative in requested:
        if relative not in supported and Path(relative).suffix != ".patch":
            fail(f"no project formatter is defined for: {relative}")
    selected = tuple(requested)
    return selected, files, formats.select(frozenset(selected))


def _container_command(  # noqa: PLR0913 -- projection and optional cache mounts stay explicit.
    kern: str,
    *,
    rootfs: Path,
    workspace: Path,
    formatter: list[str],
    archives: Path | None = None,
    linux_cache: Path | None = None,
) -> list[str]:
    return [
        kern,
        "box",
        kern_box_name("format"),
        *environment_arguments(rootfs),
        "--network",
        "none",
        "--read-only",
        "--tmpfs",
        "/tmp:256m",  # noqa: S108 -- container tmpfs.
        "--no-uid-range",
        "--volume",
        f"{workspace}:/workspace",
        "--workdir",
        "/workspace",
        "--env",
        "HOME=/tmp",
        "--env",
        "RUFF_CACHE_DIR=/tmp/ruff",
        "--env",
        "PYTHONPATH=/workspace/scripts",
        "--env",
        "PYTHONDONTWRITEBYTECODE=1",
        *(["--volume", f"{archives}:/linux-archives:ro"] if archives is not None else []),
        *(["--volume", f"{linux_cache}:/cache/linux"] if linux_cache is not None else []),
        "--init",
        "--quiet",
        "--",
        *formatter,
    ]


def _publish_outputs(
    snapshot: WorkspaceSnapshot,
    outputs: dict[str, bytes],
    *,
    root: Path,
) -> tuple[int, int]:
    before = {source.path: source for source in snapshot.files}
    for relative in outputs:
        source = before[relative]
        path = root / relative
        if _path_uses_symlink(root, relative) or not path.is_file():
            fail(f"format source changed before publication: {relative}")
        if (
            path.read_bytes() != source.contents
            or stat.S_IMODE(path.stat().st_mode) != source.mode
        ):
            fail(f"format source changed before publication: {relative}")

    changed = 0
    for relative, contents in outputs.items():
        source = before[relative]
        if contents == source.contents:
            continue
        replace_file_atomically(root / relative, contents, source.mode)
        changed += 1
    return changed, len(outputs) - changed


def format_snapshot(
    snapshot: WorkspaceSnapshot,
    selected: tuple[str, ...],
    *,
    root: Path,
    run_formatters: Callable[[Path], None],
    current_snapshot: Callable[[], WorkspaceSnapshot],
) -> tuple[int, int]:
    """Format one immutable projection and publish only after every gate succeeds."""
    outputs = canonical_outputs(snapshot, selected, run_formatters=run_formatters)
    if current_snapshot().recipe != snapshot.recipe:
        fail("source checkout changed while formatting; nothing was published")
    return _publish_outputs(snapshot, outputs, root=root)


def format_sources(values: Sequence[str]) -> None:
    """Format explicit sources in a private projection, then publish verified bytes."""
    cache = data_root(ROOT)
    inventory = quality_files(enforce_source_policy=False, selected_data_root=cache)
    selected, inventory, groups = resolve_format_paths(values, inventory=inventory)
    patches = frozenset(path for path in selected if Path(path).suffix == ".patch")
    contexts = linux_contexts(patches) if patches else ()
    snapshot = workspace_snapshot(inventory)
    archives = cache / "downloads"
    linux_cache = cache / "linux"
    fetched: set[str] = set()
    for context in contexts:
        source = resolve_source_record(context.source["source_id"], context.source)
        if source.checksum not in fetched and inspect_linux_base(cache, source.checksum) is None:
            source.acquire(cache, offline=True)
            fetched.add(source.checksum)
    if patches:
        if linux_cache.is_symlink() or (linux_cache.exists() and not linux_cache.is_dir()):
            fail(f"invalid Linux cache directory: {linux_cache}")
        linux_cache.mkdir(parents=True, exist_ok=True)
    kern, rootfs, _state = require_environment(cache=cache)

    reporter = RunReporter.create("format", target=None, verbose=False, selected_data_root=cache)
    if patches:
        print(
            "format: Linux patches: C/H only; other file types retain their contents", flush=True
        )
    with reporter.stage("sources", passthrough=not patches, show_tail=True) as stage:

        def run_formatters(projection: Path) -> None:
            for _name, command in formatter_commands(groups):
                stage.run(
                    _container_command(
                        kern,
                        rootfs=rootfs,
                        workspace=projection,
                        formatter=command,
                    ),
                    env=kern_environment(cache=cache),
                    timeout=_FORMAT_TIMEOUT_SECONDS,
                )
            if patches:
                stage.run(
                    _container_command(
                        kern,
                        rootfs=rootfs,
                        workspace=projection,
                        archives=archives,
                        linux_cache=linux_cache,
                        formatter=[
                            "python3",
                            "-m",
                            "fplinux_cli.quality.kernel_patches",
                            "--archives",
                            "/linux-archives",
                            *sorted(patches),
                        ],
                    ),
                    env=kern_environment(cache=cache),
                    timeout=_FORMAT_TIMEOUT_SECONDS,
                )

        def current_snapshot() -> WorkspaceSnapshot:
            return quality_workspace_snapshot(
                enforce_source_policy=False, selected_data_root=cache
            )

        changed, unchanged = format_snapshot(
            snapshot,
            selected,
            root=ROOT,
            run_formatters=run_formatters,
            current_snapshot=current_snapshot,
        )
    print(f"format: OK ({changed} changed, {unchanged} unchanged)")
    reporter.finish()

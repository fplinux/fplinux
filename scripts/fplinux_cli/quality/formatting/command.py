# SPDX-License-Identifier: GPL-2.0-only
"""Format explicit project sources with the pinned quality tools."""

from __future__ import annotations

import stat
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.build.kernel.state import inspect_linux_base
from fplinux_cli.build.sources import fetch
from fplinux_cli.common import ROOT, fail, relative_name, replace_file_atomically
from fplinux_cli.environment.image_store import current_image_state
from fplinux_cli.environment.images import (
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from fplinux_cli.environment.kern import (
    kern_available,
    kern_box_name,
    kern_environment,
    require_kern,
)
from fplinux_cli.environment.setup import setup
from fplinux_cli.quality.kernel_patches import linux_contexts
from fplinux_cli.reporting.run import RunReporter
from fplinux_cli.workspace.capture import WorkspaceSnapshot, workspace_snapshot
from fplinux_cli.workspace.quality_inputs import quality_files, quality_workspace_snapshot

from .canonical import canonical_outputs, formatter_commands
from .source_formats import SourceFormats, classify_source_formats

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

_FORMAT_TIMEOUT_SECONDS = 15 * 60


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
    image: str,
    workspace: Path,
    formatter: list[str],
    archives: Path | None = None,
    linux_cache: Path | None = None,
) -> list[str]:
    return [
        kern,
        "box",
        kern_box_name("format"),
        "--image",
        image,
        "--pull",
        "never",
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
    selected, inventory, groups = resolve_format_paths(values)
    patches = frozenset(path for path in selected if Path(path).suffix == ".patch")
    contexts = linux_contexts(patches) if patches else ()
    snapshot = workspace_snapshot(inventory)
    archives = ROOT / ".cache/downloads/linux"
    linux_cache = ROOT / ".cache/linux"
    fetched: set[str] = set()
    for context in contexts:
        source = context.source
        if (
            source["sha256"] not in fetched
            and inspect_linux_base(ROOT / ".cache", source["sha256"]) is None
        ):
            fetch(source["url"], source["sha256"], archives, f"linux-{source['version']}.tar.xz")
            fetched.add(source["sha256"])
    if patches:
        if linux_cache.is_symlink() or (linux_cache.exists() and not linux_cache.is_dir()):
            fail(f"invalid Linux cache directory: {linux_cache}")
        linux_cache.mkdir(parents=True, exist_ok=True)
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    image = container_image_reference(container_lock, image_recipe)
    if not kern_available(container_lock):
        setup(lock=container_lock, image_recipe=image_recipe)
    kern = require_kern(container_lock)
    if current_image_state(kern, image, image_recipe) is None:
        setup(lock=container_lock, image_recipe=image_recipe)

    reporter = RunReporter.create("format", target=None, verbose=False)
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
                        image=image,
                        workspace=projection,
                        formatter=command,
                    ),
                    env=kern_environment(),
                    timeout=_FORMAT_TIMEOUT_SECONDS,
                )
            if patches:
                stage.run(
                    _container_command(
                        kern,
                        image=image,
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
                    env=kern_environment(),
                    timeout=_FORMAT_TIMEOUT_SECONDS,
                )

        def current_snapshot() -> WorkspaceSnapshot:
            return quality_workspace_snapshot(enforce_source_policy=False)

        changed, unchanged = format_snapshot(
            snapshot,
            selected,
            root=ROOT,
            run_formatters=run_formatters,
            current_snapshot=current_snapshot,
        )
    print(f"format: OK ({changed} changed, {unchanged} unchanged)")
    reporter.finish()

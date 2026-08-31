# SPDX-License-Identifier: GPL-2.0-only
"""Format explicit project sources with the pinned quality tools."""

from __future__ import annotations

import stat
import tempfile
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from .common import ROOT, fail, relative_name, replace_file_atomically
from .config import (
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from .container import (
    current_image_state,
    kern_available,
    kern_box_name,
    kern_environment,
    require_kern,
    setup,
)
from .output import RunReporter
from .source_formats import SourceFormats, classify_source_formats
from .workspace import (
    WorkspaceSnapshot,
    quality_files,
    quality_workspace_snapshot,
    workspace_snapshot,
)

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


def _select_groups(formats: SourceFormats, selected: frozenset[str]) -> SourceFormats:
    def keep(paths: tuple[str, ...]) -> tuple[str, ...]:
        return tuple(path for path in paths if path in selected)

    return SourceFormats(
        keep(formats.python),
        keep(formats.markdown),
        keep(formats.json),
        keep(formats.toml),
        keep(formats.posix_shell),
        keep(formats.bash),
        keep(formats.c),
    )


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
        if relative not in supported:
            fail(f"no project formatter is defined for: {relative}")
    selected = tuple(requested)
    return selected, files, _select_groups(formats, frozenset(selected))


def _materialize_snapshot(snapshot: WorkspaceSnapshot, destination: Path) -> None:
    for source in snapshot.files:
        path = destination / source.path
        path.parent.mkdir(parents=True, exist_ok=True)
        written = path.write_bytes(source.contents)
        if written != len(source.contents):
            fail(f"could not write complete format projection: {source.path}")
        path.chmod(source.mode)


def _formatter_paths(paths: tuple[str, ...], workspace: str) -> list[str]:
    return [f"{workspace.rstrip('/')}/{path}" for path in paths]


def formatter_commands(
    groups: SourceFormats, *, workspace: str = "/workspace"
) -> tuple[tuple[str, list[str]], ...]:
    """Return direct pinned-tool commands for one selected source projection."""
    commands: list[tuple[str, list[str]]] = []
    if groups.toml:
        commands.append(
            ("taplo", ["taplo", "fmt", "--", *_formatter_paths(groups.toml, workspace)])
        )
    prettier = (*groups.markdown, *groups.json)
    if prettier:
        commands.append(
            (
                "prettier",
                [
                    "prettier",
                    "--write",
                    "--ignore-unknown",
                    "--",
                    *_formatter_paths(tuple(prettier), workspace),
                ],
            )
        )
    if groups.python:
        commands.append(
            ("ruff", ["ruff", "format", "--", *_formatter_paths(groups.python, workspace)])
        )
    if groups.posix_shell:
        commands.append(
            (
                "shfmt-posix",
                [
                    "shfmt",
                    "-w",
                    "-ln",
                    "posix",
                    "--",
                    *_formatter_paths(groups.posix_shell, workspace),
                ],
            )
        )
    if groups.bash:
        commands.append(
            (
                "shfmt-bash",
                [
                    "shfmt",
                    "-w",
                    "-ln",
                    "bash",
                    "--",
                    *_formatter_paths(groups.bash, workspace),
                ],
            )
        )
    if groups.c:
        commands.append(
            (
                "clang-format",
                [
                    "clang-format",
                    "--style=file",
                    "-i",
                    "--",
                    *_formatter_paths(groups.c, workspace),
                ],
            )
        )
    return tuple(commands)


def _container_command(
    kern: str,
    *,
    image: str,
    workspace: Path,
    formatter: list[str],
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
        "--init",
        "--quiet",
        "--",
        *formatter,
    ]


def _read_projection(
    root: Path,
    snapshot: WorkspaceSnapshot,
    selected: frozenset[str],
) -> dict[str, bytes]:
    expected = {source.path: source for source in snapshot.files}
    actual: set[str] = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"formatter created a symlink: {path.relative_to(root)}")
        if path.is_dir():
            continue
        if not path.is_file():
            fail(f"formatter created a non-regular file: {path.relative_to(root)}")
        actual.add(path.relative_to(root).as_posix())
    if actual != set(expected):
        fail("formatter changed the source inventory in its private projection")

    outputs: dict[str, bytes] = {}
    for relative, source in expected.items():
        path = root / relative
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode != source.mode:
            fail(f"formatter changed source permissions: {relative}")
        contents = path.read_bytes()
        if relative in selected:
            outputs[relative] = contents
        elif contents != source.contents:
            fail(f"formatter changed an unselected source: {relative}")
    return outputs


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
    with tempfile.TemporaryDirectory(prefix="fplinux-format-") as temporary:
        projection = Path(temporary)
        _materialize_snapshot(snapshot, projection)
        run_formatters(projection)
        outputs = _read_projection(projection, snapshot, frozenset(selected))
        if current_snapshot().recipe != snapshot.recipe:
            fail("source checkout changed while formatting; nothing was published")
        return _publish_outputs(snapshot, outputs, root=root)


def format_sources(values: Sequence[str]) -> None:
    """Format explicit sources in a private projection, then publish verified bytes."""
    selected, inventory, groups = resolve_format_paths(values)
    snapshot = workspace_snapshot(inventory)
    container_lock = load_container_lock()
    image_recipe = container_image_recipe_digest(container_lock)
    image = container_image_reference(container_lock, image_recipe)
    if not kern_available(container_lock):
        setup(lock=container_lock, image_recipe=image_recipe)
    kern = require_kern(container_lock)
    if current_image_state(kern, image, image_recipe) is None:
        setup(lock=container_lock, image_recipe=image_recipe)

    reporter = RunReporter.create("format", target=None, verbose=False)
    with reporter.stage("sources", passthrough=True, show_tail=True) as stage:

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

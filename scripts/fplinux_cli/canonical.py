# SPDX-License-Identifier: GPL-2.0-only
"""Compute canonical source bytes in a private, inventory-preserving projection."""

from __future__ import annotations

import argparse
import stat
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from .canonical_markdown import is_phone_readme
from .common import fail
from .source_formats import SourceFormats, source_format_kind

if TYPE_CHECKING:
    from collections.abc import Callable, Sequence

    from .workspace import WorkspaceSnapshot


def _formatter_paths(paths: tuple[str, ...], workspace: str) -> list[str]:
    return [f"{workspace.rstrip('/')}/{path}" for path in paths]


def formatter_commands(
    groups: SourceFormats, *, workspace: str = "/workspace"
) -> tuple[tuple[str, list[str]], ...]:
    """Define the same normalization and pinned formatting steps for format and check."""
    commands: list[tuple[str, list[str]]] = []
    phone_documents = tuple(path for path in groups.markdown if is_phone_readme(path))
    if phone_documents:
        # Prettier first gives equivalent heading and table spellings one recognizable form.
        commands.append(
            (
                "prettier-phone-structure",
                [
                    "prettier",
                    "--write",
                    "--parser",
                    "markdown",
                    "--",
                    *_formatter_paths(phone_documents, workspace),
                ],
            )
        )
    normalized = (
        *groups.toml,
        *groups.json,
        *groups.yaml,
        *groups.devicetree,
        *groups.text,
        *groups.markdown,
    )
    if normalized:
        commands.append(
            (
                "source-order",
                [
                    "python3",
                    "-m",
                    "fplinux_cli.canonical",
                    "--root",
                    workspace,
                    "--",
                    *normalized,
                ],
            )
        )
    if groups.toml:
        commands.append(
            ("taplo", ["taplo", "fmt", "--", *_formatter_paths(groups.toml, workspace)])
        )
    for name, paths, parser in (
        ("markdown", groups.markdown, "markdown"),
        ("json", groups.json, "json"),
        ("javascript", groups.javascript, None),
        ("yaml", groups.yaml, "yaml"),
    ):
        if not paths:
            continue
        # JSONC requires Prettier's comment-aware parser, selected by the source name.
        if name == "json":
            ordinary = tuple(path for path in paths if not path.endswith(".in"))
            templates = tuple(path for path in paths if path.endswith(".in"))
            if ordinary:
                commands.append(
                    (
                        "prettier-json",
                        [
                            "prettier",
                            "--write",
                            "--",
                            *_formatter_paths(ordinary, workspace),
                        ],
                    )
                )
            for path in templates:
                template_parser = "jsonc" if path.endswith(".jsonc.in") else "json"
                commands.append(
                    (
                        "prettier-json-template",
                        [
                            "prettier",
                            "--write",
                            "--parser",
                            template_parser,
                            "--",
                            *_formatter_paths((path,), workspace),
                        ],
                    )
                )
            continue
        commands.append(
            (
                f"prettier-{name}",
                [
                    "prettier",
                    "--write",
                    *(["--parser", parser] if parser else []),
                    "--",
                    *_formatter_paths(paths, workspace),
                ],
            )
        )
    if groups.python:
        python_paths = _formatter_paths(groups.python, workspace)
        commands.append(
            (
                "ruff-imports",
                ["ruff", "check", "--no-cache", "--select", "I001", "--fix", "--", *python_paths],
            )
        )
        commands.append(("ruff", ["ruff", "format", "--no-cache", "--", *python_paths]))
    for name, paths, dialect in (
        ("shfmt-posix", (*groups.posix_shell, *groups.posix_shell_fragments), "posix"),
        ("shfmt-bash", groups.bash, "bash"),
    ):
        if paths:
            commands.append(
                (
                    name,
                    [
                        "shfmt",
                        "-w",
                        "-ln",
                        dialect,
                        "--",
                        *_formatter_paths(tuple(paths), workspace),
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


def normalize_source(relative: str, contents: bytes) -> bytes:
    """Apply the source's ordering rules without importing quality tools on host paths."""
    kind = source_format_kind(relative)
    if kind == "toml":
        from .canonical_toml import normalize_toml  # noqa: PLC0415 -- container-only dependency.

        return normalize_toml(relative, contents)
    if kind == "yaml":
        from .canonical_yaml import normalize_yaml  # noqa: PLC0415 -- container-only dependency.

        return normalize_yaml(relative, contents)
    if kind == "json":
        from .canonical_json import normalize_json  # noqa: PLC0415 -- container-only dependency.

        return normalize_json(relative, contents)
    if kind in {"devicetree", "text"}:
        from .canonical_text import normalize_text  # noqa: PLC0415 -- format-specific import.

        return normalize_text(relative, contents)
    if kind == "markdown":
        from .canonical_markdown import normalize_markdown  # noqa: PLC0415 -- host-safe import.

        return normalize_markdown(relative, contents)
    return contents


def canonical_outputs(
    snapshot: WorkspaceSnapshot,
    selected: tuple[str, ...],
    *,
    run_formatters: Callable[[Path], None],
) -> dict[str, bytes]:
    """Return expected bytes after validating inventory, modes and unselected sources."""
    expected = {source.path: source for source in snapshot.files}
    selected_set = frozenset(selected)
    if not selected_set <= expected.keys():
        fail("canonical selection is outside the source snapshot")
    with tempfile.TemporaryDirectory(prefix="fplinux-format-") as temporary:
        projection = Path(temporary)
        for source in snapshot.files:
            path = projection / source.path
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.write_bytes(source.contents) != len(source.contents):
                fail(f"could not write complete format projection: {source.path}")
            path.chmod(source.mode)
        run_formatters(projection)
        actual: set[str] = set()
        for path in projection.rglob("*"):
            if path.is_symlink():
                fail(f"formatter created a symlink: {path.relative_to(projection)}")
            if path.is_dir():
                continue
            if not path.is_file():
                fail(f"formatter created a non-regular file: {path.relative_to(projection)}")
            actual.add(path.relative_to(projection).as_posix())
        if actual != set(expected):
            fail("formatter changed the source inventory in its private projection")
        outputs: dict[str, bytes] = {}
        for relative, source in expected.items():
            path = projection / relative
            if stat.S_IMODE(path.stat().st_mode) != source.mode:
                fail(f"formatter changed source permissions: {relative}")
            contents = path.read_bytes()
            if relative in selected_set:
                outputs[relative] = contents
            elif contents != source.contents:
                fail(f"formatter changed an unselected source: {relative}")
        return outputs


def noncanonical_paths(snapshot: WorkspaceSnapshot, outputs: dict[str, bytes]) -> tuple[str, ...]:
    """Compare canonical output with captured bytes without publishing anything."""
    before = {source.path: source.contents for source in snapshot.files}
    return tuple(sorted(path for path, contents in outputs.items() if contents != before[path]))


def main(arguments: Sequence[str] | None = None) -> None:
    """Normalize only the explicit files in one disposable source projection."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("paths", nargs="+")
    args = parser.parse_args(arguments)
    for relative in args.paths:
        path = args.root / relative
        before = path.read_bytes()
        try:
            after = normalize_source(relative, before)
        except ValueError as error:
            fail(f"cannot normalize {relative}: {error}")
        if after != before:
            path.write_bytes(after)


if __name__ == "__main__":
    from .output import run_entrypoint

    run_entrypoint(main)

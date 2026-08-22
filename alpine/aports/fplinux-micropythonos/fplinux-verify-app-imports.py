# SPDX-License-Identifier: MIT
# ruff: noqa: INP001
"""Verify direct ``from mpos import`` imports in packaged applications."""

from __future__ import annotations

import argparse
import ast
from pathlib import Path


def exported_names(module_path: Path) -> set[str]:
    """Return names bound at module scope by the package initializer."""
    tree = ast.parse(module_path.read_text(encoding="utf-8"), filename=str(module_path))
    names: set[str] = set()
    for statement in tree.body:
        if isinstance(statement, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names.add(statement.name)
        elif isinstance(statement, (ast.Import, ast.ImportFrom)):
            names.update(alias.asname or alias.name.split(".")[0] for alias in statement.names)
        elif isinstance(statement, (ast.Assign, ast.AnnAssign)):
            targets = (
                statement.targets if isinstance(statement, ast.Assign) else [statement.target]
            )
            names.update(target.id for target in targets if isinstance(target, ast.Name))
    return names


def direct_mpos_imports(app_path: Path) -> set[str]:
    """Return public names imported directly from ``mpos`` by one app."""
    names: set[str] = set()
    for source_path in sorted(app_path.rglob("*.py")):
        tree = ast.parse(source_path.read_text(encoding="utf-8"), filename=str(source_path))
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.module == "mpos":
                names.update(alias.name for alias in node.names if alias.name != "*")
    return names


def read_app_names(path: Path) -> list[str]:
    """Read the deterministic packaged-application list."""
    return [
        line
        for raw_line in path.read_text(encoding="utf-8").splitlines()
        if (line := raw_line.strip()) and not line.startswith("#")
    ]


def verify_imports(source_root: Path, app_list: Path) -> None:
    """Reject selected applications whose direct public imports are absent."""
    filesystem = source_root / "internal_filesystem"
    exports = exported_names(filesystem / "lib/mpos/__init__.py")
    failures: list[str] = []
    for app_name in read_app_names(app_list):
        app_path = filesystem / "apps" / app_name
        if not app_path.is_dir():
            failures.append(app_name + ": application directory is missing")
            continue
        missing = sorted(direct_mpos_imports(app_path) - exports)
        if missing:
            failures.append(app_name + ": missing mpos exports: " + ", ".join(missing))
    if failures:
        raise RuntimeError("; ".join(failures))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    parser.add_argument("app_list", type=Path)
    arguments = parser.parse_args()
    verify_imports(arguments.source_root, arguments.app_list)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

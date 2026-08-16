# SPDX-License-Identifier: GPL-2.0-only
"""Prove the staged module inventories cover the container import closures."""

from __future__ import annotations

import ast
import unittest
from pathlib import Path

from fplinux_cli import container, workspace

_PACKAGE = Path(workspace.__file__).resolve().parent


def _local_imports(module: str) -> set[str]:
    """Return the fplinux_cli modules one module imports directly."""
    tree = ast.parse((_PACKAGE / f"{module}.py").read_text(encoding="utf-8"))
    found: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom):
            if node.level == 1:
                if node.module:
                    found.add(node.module.split(".")[0])
                else:
                    found.update(alias.name for alias in node.names)
            elif node.module and node.module.startswith("fplinux_cli."):
                found.add(node.module.split(".")[1])
        elif isinstance(node, ast.Import):
            found.update(
                alias.name.split(".")[1]
                for alias in node.names
                if alias.name.startswith("fplinux_cli.")
            )
    return found


def _import_closure(root: str) -> set[str]:
    """Return every fplinux_cli module reachable from one entry module."""
    seen: set[str] = set()
    queue = [root]
    while queue:
        module = queue.pop()
        if module in seen:
            continue
        seen.add(module)
        queue.extend(_local_imports(module))
    return seen


class ModuleInventoryTests(unittest.TestCase):
    """Keep the hand-written module lists honest against the import graphs."""

    def test_workspace_stages_the_builder_import_closure(self) -> None:
        """Every module the container builder imports must be staged."""
        staged = set(workspace.STAGED_BUILD_SOURCES)
        needed = {f"scripts/fplinux_cli/{module}.py" for module in _import_closure("builder")} | {
            "scripts/fplinux_cli/__init__.py"
        }
        self.assertEqual(sorted(needed - staged), [])

    def test_kernel_check_ships_the_kernelcheck_import_closure(self) -> None:
        """Every module the kernel checker imports must ride in its snapshot."""
        shipped = set(container._KERNEL_IMPLEMENTATION)  # noqa: SLF001
        needed = {
            f"scripts/fplinux_cli/{module}.py" for module in _import_closure("kernelcheck")
        } | {"scripts/fplinux_cli/__init__.py"}
        self.assertEqual(sorted(needed - shipped), [])


if __name__ == "__main__":
    unittest.main()

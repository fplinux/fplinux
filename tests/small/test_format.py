# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for safe source formatting and publication."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli import format as format_module
from fplinux_cli.format import format_snapshot, resolve_format_paths
from fplinux_cli.workspace import WorkspaceSnapshot, workspace_snapshot


def _inventory(root: Path, *relative_paths: str) -> list[tuple[str, Path]]:
    return [(relative, root / relative) for relative in relative_paths]


class FormatResolutionTests(unittest.TestCase):
    """Resolve only explicit regular files from the project source inventory."""

    def test_resolver_accepts_selected_untracked_source(self) -> None:
        """A non-ignored untracked source is formattable without a tracked-file gate."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = root / "new_tool.py"
            neighbor = root / "neighbor.py"
            selected.write_text("value=1\n", encoding="utf-8")
            neighbor.write_text("other=2\n", encoding="utf-8")

            paths, _files, groups = resolve_format_paths(
                ["new_tool.py"],
                root=root,
                inventory=_inventory(root, "new_tool.py", "neighbor.py"),
            )

        self.assertEqual(paths, ("new_tool.py",))
        self.assertEqual(groups.python, ("new_tool.py",))
        self.assertNotIn("neighbor.py", groups.supported())

    def test_resolver_rejects_unsafe_or_unsupported_paths(self) -> None:
        """Reject each invalid boundary before a formatter can execute."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "notes.txt").write_text("plain\n", encoding="utf-8")
            (root / "source.py").write_text("value=1\n", encoding="utf-8")
            (root / "directory").mkdir()
            (root / "link.py").symlink_to(root / "source.py")
            inventory = _inventory(root, "notes.txt", "source.py", "link.py")
            cases = (
                (["../outside.py"], "format path must be a normalized relative path"),
                ([".cache/generated.py"], "not a project source file"),
                (["node_modules/pkg/tool.py"], "not a project source file"),
                (["directory"], "must name a regular file"),
                (["link.py"], "must not use a symlink"),
                (["notes.txt"], "no project formatter is defined"),
                (["source.py", "source.py"], "format path is duplicated"),
            )
            for paths, message in cases:
                with self.subTest(paths=paths), self.assertRaisesRegex(SystemExit, message):
                    resolve_format_paths(paths, root=root, inventory=inventory)


class FormatContainerBoundaryTests(unittest.TestCase):
    """Keep each formatter projection private to its disposable container."""

    def test_projection_is_the_only_writable_host_mount(self) -> None:
        """Mount only the private writable projection under the pinned formatter."""
        workspace = Path("/tmp/fplinux-format-projection")  # noqa: S108 -- synthetic path.
        command = format_module._container_command(  # noqa: SLF001 -- command boundary.
            "/usr/bin/kern",
            image="localhost/fplinux-build:locked",
            workspace=workspace,
            formatter=["ruff", "format", "scripts/tool.py"],
        )

        self.assertIn(f"{workspace}:/workspace", command)
        network = command.index("--network")
        self.assertEqual(command[network + 1], "none")
        self.assertEqual(command[-4:], ["--", "ruff", "format", "scripts/tool.py"])


class FormatPublicationTests(unittest.TestCase):
    """Publish only after formatter and concurrency gates succeed."""

    def test_success_changes_only_selected_bytes_and_preserves_mode(self) -> None:
        """Verified output replaces one source atomically without touching its neighbor."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = root / "selected.py"
            neighbor = root / "neighbor.py"
            selected.write_text("value=1\n", encoding="utf-8")
            neighbor.write_text("other=2\n", encoding="utf-8")
            selected.chmod(0o640)
            snapshot = workspace_snapshot(_inventory(root, "selected.py", "neighbor.py"))

            def format_selected(projection: Path) -> None:
                (projection / "selected.py").write_text("value = 1\n", encoding="utf-8")

            result = format_snapshot(
                snapshot,
                ("selected.py",),
                root=root,
                run_formatters=format_selected,
                current_snapshot=lambda: snapshot,
            )

            self.assertEqual(result, (1, 0))
            self.assertEqual(selected.read_bytes(), b"value = 1\n")
            self.assertEqual(neighbor.read_bytes(), b"other=2\n")
            self.assertEqual(selected.stat().st_mode & 0o777, 0o640)
            self.assertEqual(list(root.glob(".selected.py.*")), [])

    def test_formatter_failure_preserves_every_original(self) -> None:
        """A formatter may damage its projection without touching source bytes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = root / "selected.py"
            neighbor = root / "neighbor.py"
            selected.write_text("value=1\n", encoding="utf-8")
            neighbor.write_text("other=2\n", encoding="utf-8")
            selected.chmod(0o640)
            snapshot = workspace_snapshot(_inventory(root, "selected.py", "neighbor.py"))

            def fail_after_write(projection: Path) -> None:
                (projection / "selected.py").write_text("value = 1\n", encoding="utf-8")
                message = "formatter failed"
                raise RuntimeError(message)

            with self.assertRaisesRegex(RuntimeError, "formatter failed"):
                format_snapshot(
                    snapshot,
                    ("selected.py",),
                    root=root,
                    run_formatters=fail_after_write,
                    current_snapshot=lambda: snapshot,
                )

            self.assertEqual(selected.read_bytes(), b"value=1\n")
            self.assertEqual(neighbor.read_bytes(), b"other=2\n")
            self.assertEqual(selected.stat().st_mode & 0o777, 0o640)
            self.assertEqual(list(root.glob(".selected.py.*")), [])

    def test_concurrent_edit_is_not_overwritten(self) -> None:
        """An editor change during formatting wins and prevents publication."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = root / "selected.py"
            selected.write_text("value=1\n", encoding="utf-8")
            snapshot = workspace_snapshot(_inventory(root, "selected.py"))

            def edit_both(projection: Path) -> None:
                (projection / "selected.py").write_text("value = 1\n", encoding="utf-8")
                selected.write_text("editor = 2\n", encoding="utf-8")

            def current() -> WorkspaceSnapshot:
                return workspace_snapshot(_inventory(root, "selected.py"))

            with self.assertRaisesRegex(SystemExit, "changed while formatting"):
                format_snapshot(
                    snapshot,
                    ("selected.py",),
                    root=root,
                    run_formatters=edit_both,
                    current_snapshot=current,
                )

            self.assertEqual(selected.read_bytes(), b"editor = 2\n")


if __name__ == "__main__":
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for safe source formatting and publication."""

from __future__ import annotations

from pathlib import Path

import pytest
from fplinux_cli.quality.formatting import command as format_module
from fplinux_cli.quality.formatting.command import format_snapshot, resolve_format_paths
from fplinux_cli.workspace.capture import WorkspaceSnapshot, workspace_snapshot


def _inventory(root: Path, *relative_paths: str) -> list[tuple[str, Path]]:
    return [(relative, root / relative) for relative in relative_paths]


class FormatResolutionTests:
    """Resolve only explicit regular files from the project source inventory."""

    def test_resolver_accepts_selected_untracked_source(self, tmp_path: Path) -> None:
        """A non-ignored untracked source is formattable without a tracked-file gate."""
        root = tmp_path
        selected = root / "new_tool.py"
        neighbor = root / "neighbor.py"
        selected.write_text("value=1\n", encoding="utf-8")
        neighbor.write_text("other=2\n", encoding="utf-8")

        paths, _files, groups = resolve_format_paths(
            ["new_tool.py"],
            root=root,
            inventory=_inventory(root, "new_tool.py", "neighbor.py"),
        )

        assert (paths) == (("new_tool.py",))
        assert (groups.python) == (("new_tool.py",))
        assert ("neighbor.py") not in (groups.supported())

    @pytest.mark.parametrize(
        ("paths", "message"),
        [
            (["../outside.py"], "format path must be a normalized relative path"),
            ([".cache/generated.py"], "not a project source file"),
            (["node_modules/pkg/tool.py"], "not a project source file"),
            (["directory"], "must name a regular file"),
            (["link.py"], "must not use a symlink"),
            (["notes.txt"], "no project formatter is defined"),
            (["source.py", "source.py"], "format path is duplicated"),
        ],
        ids=[
            "parent-path",
            "cache-path",
            "dependency-path",
            "directory",
            "symlink",
            "unsupported-text",
            "duplicate",
        ],
    )
    def test_resolver_rejects_unsafe_or_unsupported_paths(
        self, tmp_path: Path, paths: list[str], message: str
    ) -> None:
        """Reject each invalid boundary before a formatter can execute."""
        root = tmp_path
        (root / "notes.txt").write_text("plain\n", encoding="utf-8")
        (root / "source.py").write_text("value=1\n", encoding="utf-8")
        (root / "directory").mkdir()
        (root / "link.py").symlink_to(root / "source.py")
        inventory = _inventory(root, "notes.txt", "source.py", "link.py")
        with pytest.raises(SystemExit, match=message):
            resolve_format_paths(paths, root=root, inventory=inventory)


class FormatContainerBoundaryTests:
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

        mounts = [command[index + 1] for index, arg in enumerate(command) if arg == "--volume"]
        assert (mounts) == ([f"{workspace}:/workspace"])
        assert ("--read-only") in (command)
        network = command.index("--network")
        assert (command[network + 1]) == ("none")
        assert (command[-4:]) == (["--", "ruff", "format", "scripts/tool.py"])

    def test_patch_formatter_reuses_linux_cache_without_writable_archive_access(self) -> None:
        """Allow bounded source preparation while keeping downloaded archives read-only."""
        command = format_module._container_command(  # noqa: SLF001 -- command boundary.
            "/usr/bin/kern",
            image="localhost/fplinux-build:locked",
            workspace=Path("/projection"),
            formatter=["python3", "-m", "fplinux_cli.quality.kernel_patches"],
            archives=Path("/downloads"),
            linux_cache=Path("/linux-cache"),
        )
        mounts = [command[index + 1] for index, arg in enumerate(command) if arg == "--volume"]
        assert (mounts) == (
            [
                "/projection:/workspace",
                "/downloads:/linux-archives:ro",
                "/linux-cache:/cache/linux",
            ]
        )
        assert ("--read-only") in (command)


class FormatPublicationTests:
    """Publish only after formatter and concurrency gates succeed."""

    def test_success_changes_only_selected_bytes_and_preserves_mode(self, tmp_path: Path) -> None:
        """Verified output replaces one source atomically without touching its neighbor."""
        root = tmp_path
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

        assert (result) == ((1, 0))
        assert (selected.read_bytes()) == (b"value = 1\n")
        assert (neighbor.read_bytes()) == (b"other=2\n")
        assert (selected.stat().st_mode & 0o777) == (0o640)
        assert (sorted(path.name for path in root.iterdir())) == (["neighbor.py", "selected.py"])

    def test_formatter_failure_preserves_every_original(self, tmp_path: Path) -> None:
        """A formatter may damage its projection without touching source bytes."""
        root = tmp_path
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

        with pytest.raises(RuntimeError, match="formatter failed"):
            format_snapshot(
                snapshot,
                ("selected.py",),
                root=root,
                run_formatters=fail_after_write,
                current_snapshot=lambda: snapshot,
            )

        assert (selected.read_bytes()) == (b"value=1\n")
        assert (neighbor.read_bytes()) == (b"other=2\n")
        assert (selected.stat().st_mode & 0o777) == (0o640)
        assert (sorted(path.name for path in root.iterdir())) == (["neighbor.py", "selected.py"])

    def test_concurrent_edit_is_not_overwritten(self, tmp_path: Path) -> None:
        """An editor change during formatting wins and prevents publication."""
        root = tmp_path
        selected = root / "selected.py"
        selected.write_text("value=1\n", encoding="utf-8")
        snapshot = workspace_snapshot(_inventory(root, "selected.py"))

        def edit_both(projection: Path) -> None:
            (projection / "selected.py").write_text("value = 1\n", encoding="utf-8")
            selected.write_text("editor = 2\n", encoding="utf-8")

        def current() -> WorkspaceSnapshot:
            return workspace_snapshot(_inventory(root, "selected.py"))

        with pytest.raises(SystemExit, match="changed while formatting"):
            format_snapshot(
                snapshot,
                ("selected.py",),
                root=root,
                run_formatters=edit_both,
                current_snapshot=current,
            )

        assert (selected.read_bytes()) == (b"editor = 2\n")

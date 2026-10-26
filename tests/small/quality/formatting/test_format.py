# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for safe source formatting and publication."""

from __future__ import annotations

import hashlib
from functools import partial
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli.manifests.source_catalog import source_build_metadata
from fplinux_cli.quality.formatting import command as format_module
from fplinux_cli.quality.formatting.command import format_snapshot, resolve_format_paths
from fplinux_cli.quality.kernel_patches import LinuxContext
from fplinux_cli.reporting import run as reporting
from fplinux_cli.workspace.capture import WorkspaceSnapshot, workspace_snapshot


def _inventory(root: Path, *relative_paths: str) -> list[tuple[str, Path]]:
    return [(relative, root / relative) for relative in relative_paths]


class FormatResolutionTests:
    """Resolve only explicit regular files from the project source inventory."""

    def test_resolver_accepts_source_from_supplied_inventory(self, tmp_path: Path) -> None:
        """A selected source in the supplied inventory can be resolved for formatting."""
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
    """Prepared container arguments restrict formatter writes to private projections."""

    def test_projection_command_has_one_private_writable_mount(self) -> None:
        """The prepared argv mounts only the projection and disables network access."""
        workspace = Path("/tmp/fplinux-format-projection")  # noqa: S108 -- synthetic path.
        command = format_module._container_command(  # noqa: SLF001 -- command boundary.
            "/usr/bin/kern",
            rootfs=Path("/environment/rootfs"),
            workspace=workspace,
            formatter=["ruff", "format", "scripts/tool.py"],
        )

        mounts = [command[index + 1] for index, arg in enumerate(command) if arg == "--volume"]
        assert (mounts) == ([f"{workspace}:/workspace"])
        assert ("--read-only") in (command)
        assert command[command.index("--rootfs") + 1] == "/environment/rootfs"
        assert "--no-uid-range" in command
        assert "--image" not in command
        assert "--pull" not in command
        network = command.index("--network")
        assert (command[network + 1]) == ("none")
        assert (command[-4:]) == (["--", "ruff", "format", "scripts/tool.py"])

    def test_patch_formatter_command_keeps_downloaded_archives_read_only(self) -> None:
        """The prepared argv allows Linux-cache writes and prohibits archive writes."""
        command = format_module._container_command(  # noqa: SLF001 -- command boundary.
            "/usr/bin/kern",
            rootfs=Path("/environment/rootfs"),
            workspace=Path("/projection"),
            formatter=["python3", "-m", "fplinux_cli.quality.kernel_patches"],
            archives=Path("/downloads"),
            linux_cache=Path("/linux-cache"),
        )
        mounts = [command[index + 1] for index, arg in enumerate(command) if arg == "--volume"]
        assert (len(mounts)) == (3)
        assert (set(mounts)) == (
            {
                "/projection:/workspace",
                "/downloads:/linux-archives:ro",
                "/linux-cache:/cache/linux",
            }
        )
        assert ("--read-only") in (command)

    def test_patch_formatting_reuses_only_the_canonical_original_cache(
        self,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """Acquisition verifies the restored original before a network-free formatter stage."""
        patch = tmp_path / "selected.patch"
        patch.write_bytes(b"patch fixture\n")
        original = tmp_path / ".cache/downloads/sources/kernel/canonical.tar.xz"
        original.parent.mkdir(parents=True)
        original.write_bytes(b"original archive\n")
        source = source_build_metadata(
            "kernel",
            {
                "version": "fixture",
                "name": "canonical.tar.xz",
                "url": "https://example.invalid/canonical.tar.xz",
                "algorithm": "sha256",
                "checksum": hashlib.sha256(b"original archive\n").hexdigest(),
            },
        )
        context = LinuxContext("fixture", "default", source, ())
        inventory = _inventory(tmp_path, "selected.patch")
        snapshot = workspace_snapshot(inventory)
        commands: list[list[str]] = []

        def record_command(_stage: reporting.Stage, command: list[str], **_kwargs: object) -> None:
            commands.append(command)

        monkeypatch.setattr(format_module, "ROOT", tmp_path)
        monkeypatch.setattr(
            format_module,
            "resolve_format_paths",
            partial(resolve_format_paths, root=tmp_path, inventory=inventory),
        )
        monkeypatch.setattr(reporting, "ROOT", tmp_path)
        monkeypatch.setattr(format_module, "quality_files", lambda **_kwargs: inventory)
        monkeypatch.setattr(
            format_module, "quality_workspace_snapshot", lambda **_kwargs: snapshot
        )
        monkeypatch.setattr(format_module, "linux_contexts", lambda _selected: (context,))
        monkeypatch.setattr(
            format_module, "require_environment", lambda: ("kern", tmp_path / "rootfs", None)
        )
        monkeypatch.setattr(format_module, "kern_environment", dict)
        monkeypatch.setattr(reporting.Stage, "run", record_command)
        with mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")):
            format_module.format_sources(["selected.patch"])

        assert original.read_bytes() == b"original archive\n"
        assert patch.read_bytes() == b"patch fixture\n"
        assert not (tmp_path / ".cache/downloads/linux").exists()
        assert len(commands) == 1
        assert f"{tmp_path / '.cache/downloads'}:/linux-archives:ro" in commands[0]
        assert commands[0][commands[0].index("--network") + 1] == "none"


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

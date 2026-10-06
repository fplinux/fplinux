# SPDX-License-Identifier: GPL-2.0-only
"""Canonical byte comparison and private-projection safety contracts."""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fplinux_cli.quality.formatting.canonical import canonical_outputs, main, noncanonical_paths
from fplinux_cli.workspace.capture import workspace_snapshot

if TYPE_CHECKING:
    from pathlib import Path


class CanonicalProjectionTests:
    """Compare expected bytes while preserving every source file and its mode."""

    def test_expected_bytes_leave_checkout_unchanged_and_detect_only_differences(
        self, tmp_path: Path
    ) -> None:
        """The read-only comparison receives the same bytes later published by format."""
        root = tmp_path
        selected = root / "selected.py"
        neighbor = root / "neighbor.py"
        selected.write_bytes(b"value=1\n")
        selected.chmod(0o640)
        neighbor.write_bytes(b"other = 2\n")
        snapshot = workspace_snapshot(
            [
                ("selected.py", selected),
                ("neighbor.py", neighbor),
            ]
        )

        def external_formatter(projection: Path) -> None:
            (projection / "selected.py").write_bytes(b"value = 1\n")

        outputs = canonical_outputs(snapshot, ("selected.py",), run_formatters=external_formatter)
        assert (outputs) == ({"selected.py": b"value = 1\n"})
        assert (noncanonical_paths(snapshot, outputs)) == (("selected.py",))
        assert (selected.read_bytes()) == (b"value=1\n")
        assert (neighbor.read_bytes()) == (b"other = 2\n")
        assert (selected.stat().st_mode & 0o777) == (0o640)

    @pytest.mark.parametrize(
        "damage",
        ["mode", "extra", "neighbor", "symlink"],
        ids=["mode", "extra", "neighbor", "symlink"],
    )
    def test_projection_damage_is_rejected_without_source_changes(
        self, tmp_path: Path, damage: str
    ) -> None:
        """A tool cannot publish permission changes, extra files or unselected edits."""
        root = tmp_path
        selected = root / "selected.py"
        neighbor = root / "neighbor.py"
        selected.write_bytes(b"value=1\n")
        neighbor.write_bytes(b"other=2\n")
        selected.chmod(0o640)
        snapshot = workspace_snapshot(
            [
                ("selected.py", selected),
                ("neighbor.py", neighbor),
            ]
        )

        def external_formatter(projection: Path, *, damage: str = damage) -> None:
            (projection / "selected.py").write_bytes(b"value = 1\n")
            if damage == "mode":
                (projection / "selected.py").chmod(0o644)
            elif damage == "extra":
                (projection / "extra.py").write_bytes(b"extra = 3\n")
            elif damage == "neighbor":
                (projection / "neighbor.py").write_bytes(b"other = 2\n")
            else:
                (projection / "extra.py").symlink_to(projection / "selected.py")

        with pytest.raises(SystemExit):
            canonical_outputs(snapshot, ("selected.py",), run_formatters=external_formatter)
        assert (selected.read_bytes()) == (b"value=1\n")
        assert (neighbor.read_bytes()) == (b"other=2\n")
        assert (selected.stat().st_mode & 0o777) == (0o640)

    def test_invalid_toml_reports_its_source_without_rewriting_it(self, tmp_path: Path) -> None:
        """Expected parsing errors retain the command's concise failure boundary."""
        root = tmp_path
        path = root / "target.toml"
        source = b'platform = "unterminated\n'
        path.write_bytes(source)
        with pytest.raises(SystemExit, match=r"^fplinux: cannot normalize target.toml:"):
            main(["--root", str(root), "--", "target.toml"])
        assert (path.read_bytes()) == (source)

# SPDX-License-Identifier: GPL-2.0-only
"""Prune only retired host-tool slots while preserving current tools and other data."""

from __future__ import annotations

import json
from typing import TYPE_CHECKING
from unittest import mock

from fplinux_cli import common
from fplinux_cli.cache.prune import builds as prune_builds
from fplinux_cli.cache.prune import operations as prune_module

if TYPE_CHECKING:
    from pathlib import Path


class HostToolPruneTests:
    """Exercise actual cache deletion using platform declarations on a temporary filesystem."""

    @staticmethod
    def test_removed_tool_is_deleted_but_current_and_unknown_data_survive(tmp_path: Path) -> None:
        """Removing a tool declaration retires its cache without deleting adjacent user data."""
        root = tmp_path
        cache = root / ".cache"
        manifest = root / "platforms/demo/platform.toml"
        manifest.parent.mkdir(parents=True)
        manifest.write_text('[host]\ntools = [{name = "reader"}, {name = "retired"}]\n')
        for name in ("reader", "retired", "notes"):
            slot = cache / "host-tools" / name
            slot.mkdir(parents=True)
            (slot / "binary").write_bytes(b"cached payload\n")
            if name != "notes":
                (slot / "receipt.json").write_text(
                    json.dumps({"recipe": "a" * 64, "sha256": "b" * 64})
                )
        with (
            mock.patch.object(common, "ROOT", root),
            mock.patch.object(prune_module, "ROOT", root),
            mock.patch.object(prune_builds, "ROOT", root),
        ):
            assert (prune_module.apply_prune(cache).removed) == (())
            manifest.write_text('[host]\ntools = [{name = "reader"}]\n')
            result = prune_module.apply_prune(cache)
        assert (result.removed) == (("host-tools/retired",))
        assert not ((cache / "host-tools/retired").exists())
        assert ((cache / "host-tools/reader/binary").read_bytes()) == (b"cached payload\n")
        assert ((cache / "host-tools/notes/binary").read_bytes()) == (b"cached payload\n")

    @staticmethod
    def test_unreadable_declarations_do_not_retire_existing_tools(tmp_path: Path) -> None:
        """A malformed platform cannot make its current cached executable disposable."""
        root = tmp_path
        manifest = root / "platforms/demo/platform.toml"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("[unfinished\n")
        cache = root / ".cache"
        slot = cache / "host-tools/reader"
        slot.mkdir(parents=True)
        (slot / "receipt.json").write_text(json.dumps({"recipe": "a" * 64, "sha256": "b" * 64}))
        (slot / "binary").write_bytes(b"keep\n")
        with (
            mock.patch.object(common, "ROOT", root),
            mock.patch.object(prune_module, "ROOT", root),
            mock.patch.object(prune_builds, "ROOT", root),
        ):
            assert (prune_module.apply_prune(cache).removed) == (())
        assert ((slot / "binary").read_bytes()) == (b"keep\n")

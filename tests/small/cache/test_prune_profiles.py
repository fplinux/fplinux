# SPDX-License-Identifier: GPL-2.0-only
"""Prune selection for removed profile caches and check receipts."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.cache.prune.operations import apply_prune, plan_prune

from tests.small.cache.prune_fixtures import patch_prune_discovery

if TYPE_CHECKING:
    from pathlib import Path


class ProfileCachePruneTests:
    """Exercise prune planning and application on isolated cache trees."""

    @staticmethod
    def test_prune_removes_only_an_orphaned_profile_cache_slot(tmp_path: Path) -> None:
        """A removed profile cannot retain a stable Kbuild work directory indefinitely."""
        cache = tmp_path / ".cache"
        current = cache / "out/nokia/profiles/usb-host-lab"
        orphan = cache / "out/nokia/profiles/removed-profile"
        for path in (current, orphan):
            path.mkdir(parents=True)
            (path / "work").mkdir()
            (path / "work/output").write_text("generated\n")

        with (
            patch_prune_discovery("discover_targets", return_value=("nokia",)),
            patch_prune_discovery("discover_profiles", return_value=("usb-host-lab",)),
        ):
            plan = plan_prune(cache)
            result = apply_prune(cache)

        decisions = {entry.path: entry.action for entry in plan.entries}
        assert (decisions["out/nokia/profiles/usb-host-lab"]) == ("protected")
        assert (decisions["out/nokia/profiles/removed-profile"]) == ("candidate")
        assert (result.removed) == (("out/nokia/profiles/removed-profile",))
        assert current.exists()
        assert not (orphan.exists())

    @staticmethod
    def test_prune_removes_only_an_orphaned_profile_check_receipt(tmp_path: Path) -> None:
        """A removed profile cannot retain a separate kernel check receipt."""
        cache = tmp_path / ".cache"
        current = cache / "check-results/profiles/usb-host-lab/kernel"
        orphan = cache / "check-results/profiles/removed-profile/kernel"
        for path in (current, orphan):
            path.mkdir(parents=True)
            (path / "success.json").write_text("generated\n")

        with (
            patch_prune_discovery("discover_targets", return_value=("nokia",)),
            patch_prune_discovery("discover_profiles", return_value=("usb-host-lab",)),
        ):
            plan = plan_prune(cache)
            result = apply_prune(cache)

        decisions = {entry.path: entry.action for entry in plan.entries}
        assert (decisions["check-results/profiles/usb-host-lab"]) == ("protected")
        assert (decisions["check-results/profiles/removed-profile"]) == ("candidate")
        assert (result.removed) == (("check-results/profiles/removed-profile",))
        assert current.exists()
        assert not (orphan.exists())

    @staticmethod
    def test_prune_removes_orphaned_profile_slots_after_a_target_is_deleted(
        tmp_path: Path,
    ) -> None:
        """No profile-only namespace can retain a removed target's generated tree."""
        cache = tmp_path / ".cache"
        paths = (
            cache / "out/deleted/profiles/host",
            cache / "analysis/sparse/deleted/profiles/host",
        )
        for path in paths:
            path.mkdir(parents=True)
            (path / "generated").write_text("generated\n")

        with (
            patch_prune_discovery("discover_targets", return_value=("current",)),
            patch_prune_discovery("discover_profiles", return_value=()),
        ):
            result = apply_prune(cache)

        assert (result.removed) == (
            (
                "analysis/sparse/deleted/profiles/host",
                "out/deleted/profiles/host",
            )
        )
        assert all(not path.exists() for path in paths)

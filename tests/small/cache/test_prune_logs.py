# SPDX-License-Identifier: GPL-2.0-only
"""Bounded command log retention and generated log cleanup."""

from __future__ import annotations

import json
from pathlib import Path

import fplinux_cli.cache.prune.operations as prune_operations
from fplinux_cli.cache.prune.operations import apply_prune, plan_prune

from tests.small.cache.prune_fixtures import patch_prune_discovery


def _cli_log(cache: Path, command: str, sequence: int, *, target: str | None = None) -> Path:
    run_id = f"20260820T0508{sequence:02d}Z-p{sequence + 100}"
    relative = Path("logs") / command
    label = command
    if target is not None:
        relative /= target
        label = f"{command} {target}"
    relative /= run_id
    path = cache / relative
    path.mkdir(parents=True)
    (path / "run.json").write_text(
        json.dumps(
            {
                "display_root": f".cache/{relative.as_posix()}",
                "label": label,
                "parent": None,
            }
        )
    )
    (path / "stage.log").write_text("log\n")
    return path


class LogPruneTests:
    """Exercise prune planning and application on isolated cache trees."""

    @staticmethod
    def test_cli_log_retention_keeps_the_newest_runs_per_command_and_target(
        tmp_path: Path,
    ) -> None:
        """Keep ten generated check/format/setup logs and builds per target."""
        cache = tmp_path / ".cache"
        check_runs = [_cli_log(cache, "check", sequence) for sequence in range(12)]
        format_runs = [_cli_log(cache, "format", sequence) for sequence in range(12)]
        setup_runs = [_cli_log(cache, "setup", sequence) for sequence in range(12)]
        first_target_runs = [
            _cli_log(cache, "build", sequence, target="target-a") for sequence in range(11)
        ]
        second_target_runs = [
            _cli_log(cache, "build", sequence, target="target-b") for sequence in range(11)
        ]

        plan = plan_prune(cache)

        assert ({entry.path for entry in plan.candidates}) == (
            {
                *(f"logs/check/{run.name}" for run in check_runs[:2]),
                *(f"logs/format/{run.name}" for run in format_runs[:2]),
                *(f"logs/setup/{run.name}" for run in setup_runs[:2]),
                f"logs/build/target-a/{first_target_runs[0].name}",
                f"logs/build/target-b/{second_target_runs[0].name}",
            }
        )
        protected = {entry.path for entry in plan.entries if entry.action == "protected"}
        assert (len(protected)) == (50)
        assert (f"logs/check/{check_runs[-1].name}") in (protected)
        assert (f"logs/format/{format_runs[-1].name}") in (protected)
        assert (f"logs/setup/{setup_runs[-1].name}") in (protected)
        assert (f"logs/build/target-a/{first_target_runs[-1].name}") in (protected)
        assert (f"logs/build/target-b/{second_target_runs[-1].name}") in (protected)

    @staticmethod
    def test_log_apply_removes_only_old_generated_nested_paths(tmp_path: Path) -> None:
        """Ignore manual, unknown, and mismatched log directories during apply."""
        cache = tmp_path / ".cache"
        check_runs = [_cli_log(cache, "check", sequence) for sequence in range(11)]
        format_runs = [_cli_log(cache, "format", sequence) for sequence in range(11)]
        build_runs = [
            _cli_log(cache, "build", sequence, target="target-a") for sequence in range(11)
        ]
        manual = cache / "logs/manual/keep-this"
        unknown_namespace = cache / "logs/imported/keep-this"
        mismatched = cache / "logs/check/20260820T060000Z-p999"
        for path in (manual, unknown_namespace, mismatched):
            path.mkdir(parents=True)
            (path / "note").write_text("keep\n")

        result = apply_prune(cache)

        assert (result.removed) == (
            (
                f"logs/build/target-a/{build_runs[0].name}",
                f"logs/check/{check_runs[0].name}",
                f"logs/format/{format_runs[0].name}",
            )
        )
        assert not (check_runs[0].exists())
        assert not (build_runs[0].exists())
        assert not (format_runs[0].exists())
        assert check_runs[-1].exists()
        assert build_runs[-1].exists()
        assert format_runs[-1].exists()
        assert manual.exists()
        assert unknown_namespace.exists()
        assert mismatched.exists()

    @staticmethod
    def test_profile_logs_have_the_same_bounded_retention(tmp_path: Path) -> None:
        """Named profile logs cannot accumulate beyond the normal per-slot limit."""
        cache = tmp_path / ".cache"
        check_runs = [
            _cli_log(cache, "check", sequence, target="profiles/usb-host-lab")
            for sequence in range(11)
        ]
        build_runs = [
            _cli_log(cache, "build", sequence, target="nokia/profiles/usb-host-lab")
            for sequence in range(11)
        ]

        with (
            patch_prune_discovery("discover_targets", return_value=("nokia",)),
            patch_prune_discovery("discover_profiles", return_value=("usb-host-lab",)),
        ):
            result = apply_prune(cache)

        assert (result.removed) == (
            (
                f"logs/build/nokia/profiles/usb-host-lab/{build_runs[0].name}",
                f"logs/check/profiles/usb-host-lab/{check_runs[0].name}",
            )
        )
        assert not (check_runs[0].exists())
        assert not (build_runs[0].exists())
        assert check_runs[-1].exists()
        assert build_runs[-1].exists()

    @staticmethod
    def test_profile_log_cleanup_keeps_ten_valid_runs_and_preserves_malformed_entries(
        tmp_path: Path,
    ) -> None:
        """Automatic retention leaves default logs and manual entries outside its selected slot."""
        cache = tmp_path / ".cache"
        default_runs = [
            _cli_log(cache, "build", sequence, target="phone") for sequence in range(11)
        ]
        runs = [
            _cli_log(cache, "build", sequence, target="phone/profiles/host")
            for sequence in range(11)
        ]
        malformed = cache / "logs/build/phone/profiles/host/manual"
        malformed.mkdir()
        (malformed / "note").write_text("keep\n")

        removed = prune_operations.discard_superseded_profile_logs(
            cache,
            "build",
            target="phone",
            profile="host",
        )

        assert (removed) == ((f"logs/build/phone/profiles/host/{runs[0].name}",))
        assert not (runs[0].exists())
        assert runs[-1].exists()
        assert malformed.exists()
        assert all(run.is_dir() for run in default_runs)

    @staticmethod
    def test_orphaned_profile_logs_are_all_disposable(tmp_path: Path) -> None:
        """A deleted target/profile does not keep even its newest host-created log."""
        cache = tmp_path / ".cache"
        check = _cli_log(cache, "check", 0, target="profiles/removed")
        build = _cli_log(cache, "build", 0, target="deleted/profiles/removed")

        with (
            patch_prune_discovery("discover_targets", return_value=("current",)),
            patch_prune_discovery("discover_profiles", return_value=()),
        ):
            result = apply_prune(cache)

        assert (result.removed) == (
            (
                "logs/build/deleted/profiles/removed",
                "logs/check/profiles/removed",
            )
        )
        assert not (check.exists())
        assert not (build.exists())

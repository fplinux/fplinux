# SPDX-License-Identifier: GPL-2.0-only
"""Prepare an exact Git tree through a controlled local CLI peer."""

from __future__ import annotations

import json
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest
from fplinux_cli import common
from fplinux_cli.ci.preflight import prepare_push, require_preflight


@dataclass(frozen=True)
class PreflightRepository:
    """Own a Git source tree and the external peer's observable results."""

    root: Path
    commit: str
    report: Path

    def commands(self) -> list[dict[str, object]]:
        """Read the external peer's completed invocation records."""
        return [json.loads(line) for line in self.report.read_text().splitlines()]


@pytest.fixture
def preflight_repository(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> PreflightRepository:
    """Replace reported disk capacity because the local peer installs no large environment."""
    capacity = shutil.disk_usage(tmp_path)._replace(
        total=40 * 1024**3, used=20 * 1024**3, free=20 * 1024**3
    )
    monkeypatch.setattr(shutil, "disk_usage", lambda _path: capacity)
    root = tmp_path / "repository"
    root.mkdir()
    entry = root / "fplinux"
    shutil.copyfile(Path(__file__).parent / "fixtures/preflight/fplinux.py", entry)
    entry.chmod(0o755)
    (root / "README.md").write_bytes(b"committed source tree\n")
    for arguments in (
        ("init", "--quiet"),
        ("config", "user.name", "Test Author"),
        ("config", "user.email", "test@example.invalid"),
        ("add", "."),
        ("commit", "--quiet", "--no-gpg-sign", "-m", "Preflight fixture"),
    ):
        subprocess.run(["git", "-C", str(root), *arguments], check=True, capture_output=True)
    commit = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    (root / "README.md").write_bytes(b"dirty source tree\n")
    cached = root / ".data/downloads/shared-original"
    cached.parent.mkdir(parents=True)
    cached.write_bytes(b"existing cache must not enter clean preparation")
    report = tmp_path / "peer-results.jsonl"
    monkeypatch.setenv("FPLINUX_PREFLIGHT_TEST_REPORT", str(report))
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(root / ".data"))
    monkeypatch.setattr(common, "ROOT", root)
    return PreflightRepository(root, commit, report)


def test_explicit_preparation_archives_the_exact_commit_and_records_only_completed_results(
    preflight_repository: PreflightRepository, tmp_path: Path
) -> None:
    repository = preflight_repository
    destination = tmp_path / "clean-preparation"

    prepare_push(repository.commit, destination)

    commands = repository.commands()
    assert [record["arguments"] for record in commands] == [
        ["dependencies", "create", str(destination / "originals"), "--inputs-only"],
        ["setup", "--offline"],
        ["doctor"],
        ["dependencies", "verify", str(destination / "originals")],
    ]
    assert all(record["checkout"] == str(destination / "checkout") for record in commands)
    assert all(
        record["data_directory"] == str(destination / "checkout/.data") for record in commands
    )
    assert all(record["readme"] == "committed source tree\n" for record in commands)
    assert commands[0]["cache_files"] == []
    assert (repository.root / "README.md").read_bytes() == b"dirty source tree\n"
    receipt = require_preflight(repository.root, repository.commit)
    assert receipt["source_commit"] == repository.commit
    assert receipt["release_id"] == repository.commit[:12]
    assert (
        receipt["snapshot"] == "363e25cc2c7c33c5cad19cb52c53de0c36f3d72c6ee3915f107258ef99f3ed1a"
    )
    assert receipt["environment_recipe"] == "a" * 64
    assert receipt["environment_content"] == "c" * 64
    assert receipt["directory"] == str(destination)
    assert receipt["completed_at"]


@pytest.mark.parametrize(
    ("failure", "command_count"),
    [("dependencies create", 1), ("setup", 2), ("doctor", 3), ("dependencies verify", 4)],
)
def test_failed_preparation_stops_following_commands_and_leaves_no_success_receipt(
    preflight_repository: PreflightRepository,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    failure: str,
    command_count: int,
) -> None:
    repository = preflight_repository
    monkeypatch.setenv("FPLINUX_PREFLIGHT_TEST_FAIL", failure)

    with pytest.raises(SystemExit, match="clean preparation stopped"):
        prepare_push(repository.commit, tmp_path / "failed-preparation")

    assert len(repository.commands()) == command_count
    assert not (repository.root / ".data/preflight" / f"{repository.commit}.json").exists()
    with pytest.raises(SystemExit, match="no completed clean preparation"):
        require_preflight(repository.root, repository.commit)


def test_clean_preparation_refuses_to_reuse_a_directory(
    preflight_repository: PreflightRepository, tmp_path: Path
) -> None:
    destination = tmp_path / "existing"
    destination.mkdir()
    marker = destination / "keep"
    marker.write_bytes(b"owned by an earlier preparation")

    with pytest.raises(SystemExit, match="requires a new directory"):
        prepare_push(preflight_repository.commit, destination)

    assert marker.read_bytes() == b"owned by an earlier preparation"
    assert not preflight_repository.report.exists()

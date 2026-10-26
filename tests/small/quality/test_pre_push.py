# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the incoming-main gate through a real push to an owned local remote."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest
from fplinux_cli.quality.pre_push import incoming_main_commits


@dataclass(frozen=True)
class PushRepository:
    """Keep the owned source tree, bare remote and process environment together."""

    root: Path
    remote: Path
    commit: str
    marker: Path
    environment: dict[str, str]

    def push(self, ref: str = "main") -> subprocess.CompletedProcess[str]:
        """Run Git's real hook protocol against only the owned local bare remote."""
        return subprocess.run(
            [
                "git",
                "-C",
                str(self.root),
                "push",
                "--porcelain",
                "origin",
                f"HEAD:refs/heads/{ref}",
            ],
            env=self.environment,
            capture_output=True,
            check=False,
            text=True,
        )

    def receipt(self, *, commit: str | None = None) -> Path:
        """Publish a controlled successful preparation for one selected source commit."""
        path = self.root / ".cache/preflight" / f"{self.commit}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(
                {
                    "source_commit": self.commit if commit is None else commit,
                    "source_snapshot": "a" * 64,
                    "snapshot": "b" * 64,
                    "environment_recipe": "c" * 64,
                    "environment_content": "d" * 64,
                    "completed_at": "2026-10-22T00:00:00+00:00",
                }
            )
        )
        return path


@pytest.fixture
def push_repository(tmp_path: Path) -> PushRepository:
    root = tmp_path / "repository"
    root.mkdir()
    remote = tmp_path / "remote.git"
    fixture = Path(__file__).parent / "fixtures/pre-push"
    source_root = Path(__file__).parents[3]
    entry = root / "fplinux"
    shutil.copyfile(fixture / "incoming.sh", entry)
    entry.chmod(0o755)
    hooks = root / ".githooks"
    hooks.mkdir()
    shutil.copyfile(source_root / ".githooks/pre-push", hooks / "pre-push")
    (hooks / "pre-push").chmod(0o755)
    for arguments in (
        ("init", "--quiet"),
        ("config", "user.name", "Test Author"),
        ("config", "user.email", "test@example.invalid"),
        ("config", "core.hooksPath", ".githooks"),
        ("add", "."),
        ("commit", "--quiet", "--no-gpg-sign", "-m", "Push fixture"),
    ):
        subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, check=True)
    subprocess.run(
        ["git", "init", "--quiet", "--bare", str(remote)], capture_output=True, check=True
    )
    subprocess.run(
        ["git", "-C", str(root), "remote", "add", "origin", str(remote)],
        capture_output=True,
        check=True,
    )
    commit = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        capture_output=True,
        check=True,
        text=True,
    ).stdout.strip()
    shutil.copyfile(fixture / "fplinux.py", entry)
    environment = os.environ.copy()
    environment["PYTHONPATH"] = str(source_root / "scripts")
    marker = tmp_path / "executed-incoming-code"
    environment["FPLINUX_PRE_PUSH_TEST_MARKER"] = str(marker)
    return PushRepository(root, remote, commit, marker, environment)


def test_pre_push_selects_only_non_deleted_incoming_main_oids() -> None:
    records = [
        f"refs/heads/topic {'a' * 40} refs/heads/topic {'b' * 40}\n",
        f"refs/heads/local {'c' * 40} refs/heads/main {'d' * 40}\n",
        f"(delete) {'0' * 40} refs/heads/main {'e' * 40}\n",
    ]

    assert incoming_main_commits(records) == ("c" * 40,)


@pytest.mark.parametrize("preparation", ["missing", "stale"])
def test_local_main_push_rejects_missing_or_another_commit_preparation(
    push_repository: PushRepository, preparation: str
) -> None:
    repository = push_repository
    if preparation == "stale":
        repository.receipt(commit="f" * 40)
    entry_before = (repository.root / "fplinux").read_bytes()

    pushed = repository.push()

    assert pushed.returncode != 0
    assert "no completed clean preparation" in pushed.stderr
    assert repository.commit in pushed.stderr
    assert not repository.marker.exists()
    assert (repository.root / "fplinux").read_bytes() == entry_before
    assert not (repository.root / ".cache/downloads").exists()
    assert not (repository.root / ".cache/environment").exists()
    refs = subprocess.run(
        ["git", "--git-dir", str(repository.remote), "show-ref", "--verify", "refs/heads/main"],
        capture_output=True,
        check=False,
    )
    assert refs.returncode != 0


def test_local_main_push_accepts_matching_receipt_without_executing_incoming_code(
    push_repository: PushRepository,
) -> None:
    repository = push_repository
    receipt = repository.receipt()
    receipt_before = receipt.read_bytes()
    index = repository.root / ".git/index"
    index_before = index.read_bytes()

    pushed = repository.push()

    assert pushed.returncode == 0, pushed.stderr
    remote = subprocess.run(
        ["git", "--git-dir", str(repository.remote), "rev-parse", "refs/heads/main"],
        capture_output=True,
        check=True,
        text=True,
    )
    assert remote.stdout.strip() == repository.commit
    assert receipt.read_bytes() == receipt_before
    assert index.read_bytes() == index_before
    assert not repository.marker.exists()
    assert not (repository.root / ".cache/downloads").exists()
    assert not (repository.root / ".cache/environment").exists()


def test_unrelated_ref_push_needs_no_preparation(push_repository: PushRepository) -> None:
    pushed = push_repository.push("topic")

    assert pushed.returncode == 0, pushed.stderr
    assert not (push_repository.root / ".cache").exists()
    assert not push_repository.marker.exists()

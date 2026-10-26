# SPDX-License-Identifier: GPL-2.0-only
"""Exercise publication with real assembled assets and a replaced GitHub process."""

from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.ci import release
from fplinux_cli.packaging.distribution import assemble_distribution

from tests.distribution_support import COMMIT, SELECTIONS, write_pair, write_transport

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def assets(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    packages = tmp_path / "packages"
    packages.mkdir()
    for target, host in SELECTIONS:
        write_pair(packages, target, host)
    transport = tmp_path / "transport"
    write_transport(transport)
    output = tmp_path / "release"
    assemble_distribution(packages, output, inputs_directory=transport)
    for name, value in {
        "GITHUB_REPOSITORY": "fplinux/fplinux",
        "GITHUB_EVENT_NAME": "push",
        "GITHUB_REF": "refs/heads/main",
        "GITHUB_SHA": COMMIT,
    }.items():
        monkeypatch.setenv(name, value)
    return output


@dataclass
class GitHubProcess:
    """The release API's durable draft state, observed at the subprocess boundary."""

    assets: Path
    records: list[dict[str, Any]] = field(default_factory=list)
    operations: list[str] = field(default_factory=list)
    uploaded: list[str] = field(default_factory=list)
    damage: str | None = None
    body: str = ""

    def remote_assets(self) -> list[dict[str, Any]]:
        """Expose uploaded assets as the remote API's independent file records."""
        return [
            {
                "name": name,
                "size": (self.assets / name).stat().st_size,
                "digest": "sha256:"
                + hashlib.sha256((self.assets / name).read_bytes()).hexdigest(),
                "state": "uploaded",
            }
            for name in self.uploaded
        ]

    def run(self, command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        """Respond to Git and release API requests using the retained draft state."""
        if command[:2] == ["git", "rev-parse"]:
            return subprocess.CompletedProcess(command, 0, COMMIT + "\n", "")
        assert command[0] == "gh"
        endpoint = command[2] if len(command) == 3 else ""
        if any("/compare/" in argument for argument in command):
            self.operations.append(command[-1])
            response: object = [
                {"commits": [{"commit": {"message": "Repair package startup\n\nDetails"}}]}
            ]
        elif "--paginate" in command:
            self.operations.append("list")
            response = [self.records]
        elif "matching-refs" in endpoint:
            self.operations.append("check-tag")
            response = []
        elif "/commits/" in endpoint:
            self.operations.append("resolve-tag")
            response = {"sha": COMMIT if endpoint.endswith("build-eeeeeeeeeeee") else "a" * 40}
        elif command[1:3] == ["release", "upload"]:
            assert command[3] == "build-eeeeeeeeeeee"
            self.operations.append("upload")
            if self.damage == "upload-failure":
                return subprocess.CompletedProcess(command, 1, "", "unavailable")
            self.uploaded.extend(path.rsplit("/", 1)[-1] for path in command[4:-2])
            response = None
        elif "POST" in command:
            self.operations.append("create-draft")
            request_body = kwargs["input"]
            assert isinstance(request_body, str)
            document = json.loads(request_body)
            assert document["target_commitish"] == COMMIT
            assert document["tag_name"] == "build-eeeeeeeeeeee"
            assert document["draft"] is True
            assert document["prerelease"] is False
            self.body = document["body"]
            record = {
                "id": 7,
                "draft": True,
                "tag_name": "build-eeeeeeeeeeee",
                "target_commitish": COMMIT,
                "assets": [],
            }
            self.records.append(record)
            response = record
        elif "PATCH" in command:
            request_body = kwargs["input"]
            assert isinstance(request_body, str)
            document = json.loads(request_body)
            if set(document) == {"body"}:
                self.operations.append("refresh-notes")
                self.body = document["body"]
                return subprocess.CompletedProcess(command, 0, "{}", "")
            self.operations.append("publish")
            assert document == {"draft": False, "prerelease": False, "make_latest": "true"}
            response = {
                "draft": False,
                "prerelease": False,
                "html_url": "https://example.invalid/release",
            }
        else:
            self.operations.append("verify-draft")
            uploaded = self.remote_assets()
            if self.damage == "missing-remote":
                uploaded.pop()
            elif self.damage == "wrong-digest":
                uploaded[0]["digest"] = "sha256:" + "0" * 64
            response = {
                "id": 7,
                "draft": True,
                "tag_name": "build-eeeeeeeeeeee",
                "target_commitish": COMMIT,
                "assets": uploaded,
            }
        return subprocess.CompletedProcess(
            command, 0, json.dumps(response) if response is not None else "", ""
        )


@pytest.fixture
def github(assets: Path, monkeypatch: pytest.MonkeyPatch) -> GitHubProcess:
    process = GitHubProcess(assets)
    monkeypatch.setattr(subprocess, "run", process.run)
    return process


def test_initial_release_publishes_only_after_all_remote_assets_match(
    assets: Path, github: GitHubProcess
) -> None:
    release.publish_distribution(assets, commit=COMMIT)
    assert github.operations == [
        "list",
        "check-tag",
        "create-draft",
        "upload",
        "verify-draft",
        "publish",
    ]
    assert github.body.startswith("Initial release.")
    assert set(github.uploaded) == {path.name for path in assets.iterdir()}


@pytest.mark.parametrize("damage", ["upload-failure", "missing-remote", "wrong-digest"])
def test_remote_failure_keeps_the_release_private(
    assets: Path, github: GitHubProcess, damage: str
) -> None:
    github.damage = damage
    with pytest.raises(SystemExit):
        release.publish_distribution(assets, commit=COMMIT)
    assert "create-draft" in github.operations
    assert "publish" not in github.operations


def test_retry_of_a_complete_published_release_only_verifies_bytes(
    assets: Path, github: GitHubProcess
) -> None:
    github.uploaded = sorted(path.name for path in assets.iterdir())
    github.records.append(
        {
            "id": 7,
            "draft": False,
            "tag_name": "build-eeeeeeeeeeee",
            "assets": github.remote_assets(),
            "published_at": "2026-10-22T10:00:00Z",
        }
    )
    release.publish_distribution(assets, commit=COMMIT)
    assert github.operations == ["list", "resolve-tag"]


def test_retry_fills_only_missing_matching_draft_assets(
    assets: Path, github: GitHubProcess
) -> None:
    github.uploaded = sorted(path.name for path in assets.iterdir())[:-1]
    missing = {path.name for path in assets.iterdir()} - set(github.uploaded)
    github.records.append(
        {
            "id": 7,
            "draft": True,
            "tag_name": "build-eeeeeeeeeeee",
            "target_commitish": COMMIT,
            "assets": github.remote_assets(),
        }
    )
    release.publish_distribution(assets, commit=COMMIT)
    assert github.operations == ["list", "refresh-notes", "upload", "verify-draft", "publish"]
    assert set(github.uploaded) >= missing
    assert len(github.uploaded) == len(list(assets.iterdir()))


def test_notes_use_the_last_actual_publication_and_its_resolved_commit(
    assets: Path, github: GitHubProcess
) -> None:
    github.records.extend(
        [
            {"id": 2, "draft": False, "tag_name": "older", "published_at": "2026-10-20T09:00:00Z"},
            {
                "id": 3,
                "draft": False,
                "tag_name": "previous",
                "published_at": "2026-10-21T09:00:00Z",
            },
            {"id": 4, "draft": True, "tag_name": "unfinished"},
        ]
    )
    release.publish_distribution(assets, commit=COMMIT)
    assert "Changes since previous:" in github.body
    assert "Repair package startup" in github.body
    assert f"compare/{'a' * 40}...{COMMIT}" in github.body


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("GITHUB_REPOSITORY", "fork/fplinux"),
        ("GITHUB_EVENT_NAME", "workflow_dispatch"),
        ("GITHUB_REF", "refs/heads/topic"),
        ("GITHUB_SHA", "0" * 40),
    ],
)
def test_untrusted_event_prevents_remote_operations(
    assets: Path, github: GitHubProcess, monkeypatch: pytest.MonkeyPatch, name: str, value: str
) -> None:
    monkeypatch.setenv(name, value)
    with pytest.raises(SystemExit, match="canonical main push"):
        release.publish_distribution(assets, commit=COMMIT)
    assert github.operations == []

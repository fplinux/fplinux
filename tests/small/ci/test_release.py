# SPDX-License-Identifier: GPL-2.0-only
"""Exercise publication with real assembled assets and a replaced GitHub process."""

from __future__ import annotations

import hashlib
import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.ci import release
from fplinux_cli.packaging.distribution import attach_inputs

from tests import distribution_support
from tests.distribution_support import SELECTIONS, project_sources, write_release, write_transport

if TYPE_CHECKING:
    from collections.abc import Callable, Iterable


@pytest.fixture
def checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    root = tmp_path / "checkout"
    root.mkdir()
    for name, data in project_sources().items():
        path = root / name.removeprefix("project/")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (root / "fplinux").chmod(0o755)
    for arguments in (
        ("init", "--quiet"),
        ("add", "."),
        (
            "-c",
            "user.name=Test Author",
            "-c",
            "user.email=test@example.invalid",
            "commit",
            "--quiet",
            "--no-gpg-sign",
            "-m",
            "Publication source",
        ),
        (
            "-c",
            "user.name=Test Author",
            "-c",
            "user.email=test@example.invalid",
            "commit",
            "--quiet",
            "--no-gpg-sign",
            "--allow-empty",
            "-m",
            "Publication event",
        ),
    ):
        subprocess.run(["git", *arguments], cwd=root, check=True, capture_output=True)
    monkeypatch.setattr(common, "ROOT", root)
    monkeypatch.setattr(release, "distribution_matrix", lambda: SELECTIONS)
    return root


@pytest.fixture
def commit(checkout: Path) -> str:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=checkout, text=True).strip()


@pytest.fixture
def assets(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, commit: str) -> Path:
    for name, value in {
        "GITHUB_REPOSITORY": "fplinux/fplinux",
        "GITHUB_EVENT_NAME": "push",
        "GITHUB_REF": "refs/heads/main",
        "GITHUB_SHA": commit,
    }.items():
        monkeypatch.setenv(name, value)
    return _write_assets(tmp_path, commit)


def _write_assets(directory: Path, commit: str) -> Path:
    packages = directory / "packages"
    write_release(packages, commit=commit)
    transport = directory / "transport"
    write_transport(transport, commit=commit)
    output = directory / "release"
    attach_inputs(packages, transport, output, expected_matrix=SELECTIONS)
    return output


@dataclass
class GitHubProcess:
    """The release API's durable draft state, observed at the subprocess boundary."""

    assets: Path
    checkout: Path
    commit: str
    real_run: Callable[..., subprocess.CompletedProcess[str]]
    tag: str
    prerelease: bool = True
    records: list[dict[str, Any]] = field(default_factory=list)
    operations: list[str] = field(default_factory=list)
    uploaded: list[tuple[str, bytes]] = field(default_factory=list)
    damage: str | None = None
    body: str = ""
    publication: dict[str, object] | None = None
    name: str = ""

    def upload(self, paths: Iterable[Path]) -> None:
        """Retain the bytes from each transferred path at the upload boundary."""
        self.uploaded.extend((path.name, path.read_bytes()) for path in paths)

    def remote_assets(self) -> list[dict[str, Any]]:
        """Expose uploaded assets as the remote API's independent file records."""
        return [
            {
                "name": name,
                "size": len(contents),
                "digest": "sha256:" + hashlib.sha256(contents).hexdigest(),
                "state": "uploaded",
            }
            for name, contents in self.uploaded
        ]

    def run(self, command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        """Respond to Git and release API requests using the retained draft state."""
        if command[0] == "git":
            return self.real_run(command, **kwargs)
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
        elif "/git/ref/tags/" in endpoint:
            self.operations.append("resolve-tag")
            tag = endpoint.split("/git/ref/tags/", 1)[1]
            if tag == self.tag and self.damage == "tag-changed" and "upload" in self.operations:
                sha, kind = "a" * 40, "commit"
            elif tag == f"build-{self.commit[:12]}":
                sha, kind = self.commit, "commit"
            else:
                result = self.real_run(
                    ["git", "rev-parse", "--verify", f"refs/tags/{tag}"],
                    cwd=self.checkout,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                if result.returncode:
                    return subprocess.CompletedProcess(command, 1, "", "tag not found")
                sha = result.stdout.strip()
                kind = self.real_run(
                    ["git", "cat-file", "-t", sha],
                    cwd=self.checkout,
                    capture_output=True,
                    text=True,
                    check=True,
                ).stdout.strip()
            response = {"ref": f"refs/tags/{tag}", "object": {"sha": sha, "type": kind}}
        elif "/git/tags/" in endpoint:
            sha = endpoint.rsplit("/", 1)[-1]
            tag_object = self.real_run(
                ["git", "cat-file", "tag", sha],
                cwd=self.checkout,
                capture_output=True,
                text=True,
                check=True,
            ).stdout.splitlines()
            assert tag_object[0].startswith("object ")
            assert tag_object[1].startswith("type ")
            response = {
                "sha": sha,
                "object": {
                    "sha": tag_object[0].removeprefix("object "),
                    "type": tag_object[1].removeprefix("type "),
                },
            }
        elif command[1:3] == ["release", "upload"]:
            assert command[3] == self.tag
            self.operations.append("upload")
            if self.damage == "upload-failure":
                return subprocess.CompletedProcess(command, 1, "", "unavailable")
            self.upload(map(Path, command[4:-2]))
            response = None
        elif "POST" in command:
            self.operations.append("create-draft")
            request_body = kwargs["input"]
            assert isinstance(request_body, str)
            document = json.loads(request_body)
            assert document["target_commitish"] == self.commit
            assert document["tag_name"] == self.tag
            assert document["draft"] is True
            assert document["prerelease"] is self.prerelease
            self.body = document["body"]
            self.name = document["name"]
            record = {
                "id": 7,
                "draft": True,
                "tag_name": self.tag,
                "target_commitish": self.commit if self.prerelease else "main",
                "prerelease": self.prerelease,
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
            self.publication = document
            response = {
                "draft": False,
                "prerelease": document["prerelease"],
                "tag_name": self.tag,
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
                "tag_name": self.tag,
                "target_commitish": self.commit if self.prerelease else "main",
                "prerelease": self.prerelease,
                "assets": uploaded,
            }
        return subprocess.CompletedProcess(
            command, 0, json.dumps(response) if response is not None else "", ""
        )


@pytest.fixture
def github(
    assets: Path, checkout: Path, commit: str, monkeypatch: pytest.MonkeyPatch
) -> GitHubProcess:
    process = GitHubProcess(assets, checkout, commit, subprocess.run, f"build-{commit[:12]}")
    monkeypatch.setattr(subprocess, "run", process.run)
    return process


@pytest.fixture
def stable(github: GitHubProcess, monkeypatch: pytest.MonkeyPatch) -> GitHubProcess:
    github.tag = "v1.0.0"
    github.prerelease = False
    github.real_run(
        [
            "git",
            "-c",
            "user.name=Test Author",
            "-c",
            "user.email=test@example.invalid",
            "tag",
            "--no-sign",
            "-m",
            "Stable test tag",
            "v1.0.0",
            github.commit,
        ],
        cwd=github.checkout,
        check=True,
        capture_output=True,
    )
    monkeypatch.setenv("GITHUB_REF", "refs/tags/v1.0.0")
    return github


def test_initial_release_publishes_only_after_all_remote_assets_match(
    assets: Path, github: GitHubProcess, commit: str
) -> None:
    release.publish_distribution(assets, commit=commit)
    assert github.operations == [
        "list",
        "check-tag",
        "create-draft",
        "upload",
        "verify-draft",
        "publish",
    ]
    assert github.publication == {"draft": False, "prerelease": True, "make_latest": "false"}
    assert github.body.startswith("Initial release.")
    assert {name for name, _ in github.uploaded} == {path.name for path in assets.iterdir()}


@pytest.mark.parametrize("damage", ["upload-failure", "missing-remote", "wrong-digest"])
def test_remote_failure_keeps_the_release_private(
    assets: Path, github: GitHubProcess, damage: str, commit: str
) -> None:
    github.damage = damage
    with pytest.raises(SystemExit):
        release.publish_distribution(assets, commit=commit)
    assert "create-draft" in github.operations
    assert "publish" not in github.operations


def test_retry_of_a_complete_published_release_only_verifies_bytes(
    assets: Path, github: GitHubProcess, commit: str
) -> None:
    github.upload(sorted(github.assets.iterdir()))
    github.records.append(
        {
            "id": 7,
            "draft": False,
            "tag_name": f"build-{commit[:12]}",
            "prerelease": True,
            "assets": github.remote_assets(),
            "published_at": "2026-10-22T10:00:00Z",
        }
    )
    release.publish_distribution(assets, commit=commit)
    assert github.operations == ["list", "resolve-tag"]


def test_retry_fills_only_missing_matching_draft_assets(
    assets: Path, stable: GitHubProcess, commit: str
) -> None:
    github = stable
    github.upload(sorted(github.assets.iterdir())[:-1])
    missing = {path.name for path in assets.iterdir()} - {name for name, _ in github.uploaded}
    github.records.append(
        {
            "id": 7,
            "draft": True,
            "tag_name": "v1.0.0",
            "target_commitish": "main",
            "prerelease": False,
            "assets": github.remote_assets(),
        }
    )
    release.publish_distribution(assets, commit=commit)
    assert github.operations == [
        "resolve-tag",
        "list",
        "refresh-notes",
        "upload",
        "verify-draft",
        "resolve-tag",
        "publish",
    ]
    assert {name for name, _ in github.uploaded} >= missing
    assert len(github.uploaded) == len(list(assets.iterdir()))


def test_stable_tag_publishes_with_notes_from_the_previous_stable_version(
    assets: Path, stable: GitHubProcess, commit: str
) -> None:
    previous_commit = stable.real_run(
        ["git", "rev-parse", "HEAD^"],
        cwd=stable.checkout,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    stable.real_run(
        ["git", "update-ref", "refs/tags/v0.9.10", previous_commit],
        cwd=stable.checkout,
        capture_output=True,
        check=True,
    )
    stable.records.extend(
        [
            {"id": 2, "draft": False, "prerelease": True, "tag_name": "build-aaaaaaaaaaaa"},
            {
                "id": 3,
                "draft": False,
                "prerelease": False,
                "tag_name": "v0.9.10",
                "published_at": "2026-10-20T09:00:00Z",
            },
            {
                "id": 4,
                "draft": False,
                "prerelease": False,
                "tag_name": "v0.9.9",
                "published_at": "2026-10-21T09:00:00Z",
            },
            {"id": 5, "draft": True, "prerelease": False, "tag_name": "v0.10.0"},
            {"id": 6, "draft": False, "prerelease": True, "tag_name": "v0.11.0"},
            {"id": 7, "draft": False, "prerelease": False, "tag_name": "v1.1.0"},
            {"id": 8, "draft": False, "prerelease": False, "tag_name": "build-bbbbbbbbbbbb"},
        ]
    )
    release.publish_distribution(assets, commit=commit)
    assert stable.name == "FPLinux 1.0.0"
    assert stable.publication == {"draft": False, "prerelease": False, "make_latest": "true"}
    assert "Changes since v0.9.10:" in stable.body
    assert "Repair package startup" in stable.body
    assert f"compare/{previous_commit}...{commit}" in stable.body
    assert "check-tag" not in stable.operations


def test_different_declared_version_stops_publication_before_remote_access(
    assets: Path, stable: GitHubProcess, commit: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("GITHUB_REF", "refs/tags/v1.0.1")
    with pytest.raises(SystemExit, match=r"does not match the declared release v1\.0\.0"):
        release.publish_distribution(assets, commit=commit)
    assert stable.operations == []


def test_stable_tag_changed_after_upload_stays_private(
    assets: Path, stable: GitHubProcess, commit: str
) -> None:
    stable.damage = "tag-changed"
    with pytest.raises(SystemExit, match="stable release tag changed before publication"):
        release.publish_distribution(assets, commit=commit)
    assert "upload" in stable.operations
    assert stable.publication is None


def test_publisher_rejects_dirty_event_checkout(
    assets: Path, github: GitHubProcess, checkout: Path
) -> None:
    (checkout / "VERSION").write_bytes(b"1.0.1\n")
    with pytest.raises(SystemExit, match="clean checkout of the exact event commit"):
        release.publish_distribution(assets, commit=github.commit)
    assert github.operations == []


def test_publisher_rejects_self_consistent_assets_from_different_source(
    github: GitHubProcess, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Complete packages cannot claim the event SHA with a different source snapshot."""
    different = tmp_path / "different"
    different.mkdir()
    sources = {**project_sources(), "project/common/example.c": b"different source\n"}
    monkeypatch.setattr(distribution_support, "project_sources", lambda: sources)
    assets = _write_assets(different, github.commit)
    with pytest.raises(
        SystemExit, match="source association differs from the exact event checkout"
    ):
        release.publish_distribution(assets, commit=github.commit)
    assert github.operations == []


def test_stable_tag_pointing_to_another_commit_is_rejected_before_draft(
    assets: Path, stable: GitHubProcess
) -> None:
    stable.real_run(
        ["git", "update-ref", "refs/tags/v1.0.0", "HEAD^"],
        cwd=stable.checkout,
        capture_output=True,
        check=True,
    )
    with pytest.raises(SystemExit, match="stable release tag belongs to another source commit"):
        release.publish_distribution(assets, commit=stable.commit)
    assert stable.operations == ["resolve-tag"]


@pytest.mark.parametrize(
    ("name", "value"),
    [
        ("GITHUB_REPOSITORY", "fork/fplinux"),
        ("GITHUB_EVENT_NAME", "workflow_dispatch"),
        ("GITHUB_REF", "refs/heads/topic"),
        ("GITHUB_REF", "refs/tags/v01.0.0"),
        ("GITHUB_REF", "refs/tags/v1.0.0-rc.1"),
        ("GITHUB_SHA", "0" * 40),
    ],
)
def test_untrusted_event_prevents_remote_operations(
    assets: Path, github: GitHubProcess, monkeypatch: pytest.MonkeyPatch, name: str, value: str
) -> None:
    monkeypatch.setenv(name, value)
    with pytest.raises(SystemExit, match="canonical main or stable tag push"):
        release.publish_distribution(assets, commit=github.commit)
    assert github.operations == []

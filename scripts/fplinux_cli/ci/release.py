# SPDX-License-Identifier: GPL-2.0-only
"""Publish one complete verified release through a private draft."""

from __future__ import annotations

import json
import os
import re
import subprocess
from typing import TYPE_CHECKING, Any, cast

from fplinux_cli import common
from fplinux_cli.artifacts.release import release_context_from_fields
from fplinux_cli.cli.inspect import inspect_distribution
from fplinux_cli.common import fail

if TYPE_CHECKING:
    from pathlib import Path

CANONICAL_REPOSITORY = "fplinux/fplinux"
MAX_ASSET_BYTES = 2 * 1024**3


def _gh(arguments: list[str], *, document: dict[str, object] | None = None) -> object:
    """Keep credentials in gh's environment and return only decoded API data."""
    command = ["gh", *arguments]
    if document is not None:
        command.extend(["--input", "-"])
    result = subprocess.run(
        command,
        input=json.dumps(document) if document is not None else None,
        capture_output=True,
        text=True,
        check=False,
        timeout=1800,
    )
    if result.returncode:
        fail("GitHub release operation failed; publication stopped")
    if not result.stdout.strip():
        return None
    try:
        decoded: object = json.loads(result.stdout)
    except ValueError:
        fail("GitHub returned invalid release metadata")
    return decoded


def _require_publisher(commit: str) -> None:
    if (
        os.environ.get("GITHUB_REPOSITORY") != CANONICAL_REPOSITORY
        or os.environ.get("GITHUB_EVENT_NAME") != "push"
        or os.environ.get("GITHUB_REF") != "refs/heads/main"
        or os.environ.get("GITHUB_SHA") != commit
    ):
        fail("publication requires a canonical main push at its exact event commit")
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        fail("publication requires the complete Git commit SHA")
    head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=common.ROOT,
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )
    if head.returncode or head.stdout.strip() != commit:
        fail("publisher checkout differs from the event commit")


def _verify_remote_assets(
    release: dict[str, Any], assets: dict[str, tuple[int, str]], *, complete: bool
) -> list[str]:
    """Verify existing bytes before permitting a missing-only draft upload."""
    records = release.get("assets")
    if not isinstance(records, list):
        fail("release has invalid asset metadata")
    seen: set[str] = set()
    for record in records:
        if not isinstance(record, dict):
            fail("release contains invalid asset metadata")
        name = record.get("name")
        if not isinstance(name, str) or name in seen or name not in assets:
            fail("release contains an unexpected or duplicate asset")
        seen.add(name)
        size, digest = assets[name]
        if (
            record.get("state") != "uploaded"
            or record.get("size") != size
            or record.get("digest") != f"sha256:{digest}"
        ):
            fail(f"release asset size or SHA-256 differs: {name}")
    missing = sorted(assets.keys() - seen)
    if complete and missing:
        fail("release has an incomplete asset inventory")
    return missing


def _release_list(endpoint: str) -> list[dict[str, Any]]:
    pages = _gh(["api", "--paginate", "--slurp", f"{endpoint}/releases?per_page=100"])
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        fail("GitHub returned an invalid release inventory")
    records = [record for page in pages for record in page]
    if any(
        not isinstance(record, dict)
        or type(record.get("id")) is not int
        or not isinstance(record.get("tag_name"), str)
        or type(record.get("draft")) is not bool
        for record in records
    ):
        fail("GitHub returned invalid release records")
    return cast("list[dict[str, Any]]", records)


def _tag_commit(endpoint: str, tag: str) -> str:
    record = _gh(["api", f"{endpoint}/commits/{tag}"])
    if not isinstance(record, dict) or not isinstance(record.get("sha"), str):
        fail("cannot resolve a release tag to its source commit")
    sha: str = record["sha"]
    if re.fullmatch(r"[0-9a-f]{40}", sha) is None:
        fail("release tag did not resolve to a complete source commit")
    return sha


def _release_notes(endpoint: str, releases: list[dict[str, Any]], commit: str) -> str:
    published = [record for record in releases if not record["draft"]]
    if any(not isinstance(record.get("published_at"), str) for record in published):
        fail("published release metadata has no publication time")
    if not published:
        return f"Initial release.\n\nSource commit: {commit}.\n\nVerify downloads with SHA256SUMS."
    previous = max(published, key=lambda record: record["published_at"])
    previous_commit = _tag_commit(endpoint, previous["tag_name"])
    pages = _gh(
        [
            "api",
            "--paginate",
            "--slurp",
            f"{endpoint}/compare/{previous_commit}...{commit}?per_page=100",
        ]
    )
    if (
        not isinstance(pages, list)
        or not pages
        or any(
            not isinstance(page, dict) or not isinstance(page.get("commits"), list)
            for page in pages
        )
    ):
        fail("GitHub returned an invalid release change range")
    changes = []
    for item in [item for page in pages for item in page["commits"]]:
        if not isinstance(item, dict) or not isinstance(item.get("commit"), dict):
            fail("GitHub returned an invalid release change")
        message = item["commit"].get("message")
        if not isinstance(message, str) or not message.strip():
            fail("GitHub returned an empty release change")
        changes.append(f"- {message.splitlines()[0]}")
    body = "\n".join(changes) or "No commits added since the previous release."
    compare = f"https://github.com/{CANONICAL_REPOSITORY}/compare/{previous_commit}...{commit}"
    return (
        f"Changes since {previous['tag_name']}:\n\n{body}\n\n"
        f"[Full changes]({compare})\n\nSource commit: {commit}.\n\n"
        "Verify downloads with SHA256SUMS."
    )


def publish_distribution(directory: Path, *, commit: str) -> None:
    """Publish after the complete local and remote asset inventories match."""
    _require_publisher(commit)
    inspect_distribution(directory)
    context = release_context_from_fields(json.loads((directory / "RELEASE.json").read_bytes()))
    if context.source_commit != commit or context.release_id != commit[:12]:
        fail("publication source association differs from the exact event commit")
    files = sorted(directory.iterdir())
    if any(not path.is_file() or path.is_symlink() for path in files):
        fail("publication requires regular distribution assets")
    assets: dict[str, tuple[int, str]] = {}
    for path in files:
        size = path.stat().st_size
        if size >= MAX_ASSET_BYTES:
            fail(f"release asset reaches GitHub's 2 GiB limit: {path.name}")
        assets[path.name] = (size, common.sha256_file(path))
    endpoint = f"repos/{CANONICAL_REPOSITORY}"
    tag = context.tag
    releases = _release_list(endpoint)
    matches = [record for record in releases if record["tag_name"] == tag]
    if len(matches) > 1:
        fail("GitHub repeats this release identity")
    if matches:
        release = matches[0]
        if release["draft"]:
            if release.get("target_commitish") != commit:
                fail("existing draft belongs to another source commit")
        elif _tag_commit(endpoint, tag) != commit:
            fail("published release tag belongs to another source commit")
    else:
        tags = _gh(["api", f"{endpoint}/git/matching-refs/tags/{tag}"])
        if not isinstance(tags, list) or any(
            not isinstance(record, dict) or record.get("ref") == f"refs/tags/{tag}"
            for record in tags
        ):
            fail("publication tag already exists without its matching release")
        created = _gh(
            ["api", "--method", "POST", f"{endpoint}/releases"],
            document={
                "tag_name": tag,
                "target_commitish": commit,
                "name": f"FPLinux {context.release_id}",
                "body": _release_notes(endpoint, releases, commit),
                "draft": True,
                "prerelease": False,
            },
        )
        if (
            not isinstance(created, dict)
            or type(created.get("id")) is not int
            or created.get("draft") is not True
        ):
            fail("GitHub did not create a private draft release")
        release = created
    missing = _verify_remote_assets(release, assets, complete=not release["draft"])
    if not release["draft"]:
        print(f"Verified published release: {release.get('html_url', context.release_id)}")
        return
    if matches:
        _gh(
            ["api", "--method", "PATCH", f"{endpoint}/releases/{release['id']}"],
            document={"body": _release_notes(endpoint, releases, commit)},
        )
    if missing:
        _gh(
            [
                "release",
                "upload",
                tag,
                *(str(directory / name) for name in missing),
                "--repo",
                CANONICAL_REPOSITORY,
            ]
        )
    uploaded = _gh(["api", f"{endpoint}/releases/{release['id']}"])
    if (
        not isinstance(uploaded, dict)
        or uploaded.get("draft") is not True
        or uploaded.get("tag_name") != tag
        or uploaded.get("target_commitish") != commit
    ):
        fail("draft release identity changed before publication")
    _verify_remote_assets(uploaded, assets, complete=True)
    published = _gh(
        ["api", "--method", "PATCH", f"{endpoint}/releases/{release['id']}"],
        document={"draft": False, "prerelease": False, "make_latest": "true"},
    )
    if (
        not isinstance(published, dict)
        or published.get("draft") is not False
        or published.get("prerelease") is not False
    ):
        fail("GitHub did not confirm the published release")
    print(f"Published release: {published.get('html_url', context.release_id)}")

# SPDX-License-Identifier: GPL-2.0-only
"""Fetch and project pinned upstream sources."""

from __future__ import annotations

import shutil
import tarfile
import tempfile
import urllib.request
from pathlib import Path
from typing import Any

from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.manifests.values import relative_value


def fetch(url: object, expected: object, cache: Path, name: object) -> Path:
    """Fetch one HTTPS resource into the validated shared download cache."""
    if not isinstance(url, str) or not url.startswith("https://"):
        fail("source URL must be a non-empty HTTPS URL")
    digest = inputs_build.require_sha256(expected, f"{name} source")
    relative = relative_value(name, "download cache name")
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_file():
            fail(f"download cache destination is invalid: {destination}")
        if sha256_file(destination) == digest:
            return destination
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent,
            prefix=f".{destination.name}.",
            delete=False,
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(  # noqa: S310 -- HTTPS is required above.
                url,
                headers={"User-Agent": "FPLinux"},
            )
            with urllib.request.urlopen(  # noqa: S310 -- HTTPS is required above.
                request,
                timeout=60,
            ) as response:
                shutil.copyfileobj(response, output)
        actual = sha256_file(temporary)
        if actual != digest:
            fail(f"{name} SHA256 mismatch: expected {digest}, received {actual}")
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return destination


def source_lock_entry(sources: dict[str, Any], name: object) -> dict[str, Any]:
    """Resolve a named source-lock entry without allowing a path or command."""
    if not isinstance(name, str) or not name:
        fail("source lock key must be a non-empty string")
    value = sources.get(name)
    if not isinstance(value, dict):
        fail(f"source lock entry is missing: {name}")
    return value


def write_generated_files(root: Path, files: dict[str, bytes], *, owner: str) -> None:
    """Write generated files into one private projection without replacing source."""
    for relative, contents in sorted(files.items()):
        destination = root / relative_value(relative, f"{owner} generated path")
        if destination.is_symlink() or destination.exists():
            fail(f"{owner} generated path collides with projected source: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(contents)


def apply_patches(source: Path, paths: list[Path]) -> None:
    """Apply ordered, fuzz-free patches to one verified source projection."""
    for patch in paths:
        process_build.run(
            ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(patch)],
            cwd=source,
        )


def copy_steps(source: Path, steps: list[tuple[Path, str]]) -> None:
    """Project source files into a prepared Linux tree."""
    for source_path, destination_name in steps:
        destination = source / relative_value(destination_name, "Linux copy destination")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(inputs_build.require_file(source_path), destination)


def append_steps(source: Path, steps: list[tuple[Path, str]]) -> None:
    """Append Kconfig/Kbuild fragments in declared order."""
    for source_path, destination_name in steps:
        relative = relative_value(destination_name, "Linux append destination")
        destination = inputs_build.require_file(source / relative)
        with destination.open("ab") as output:
            output.write(b"\n")
            output.write(inputs_build.require_file(source_path).read_bytes())


def resolve_steps(
    target: str,
    steps: list[dict[str, Any]],
    *,
    platform_owned: bool,
) -> list[tuple[Path, str]]:
    """Resolve typed Linux projection steps from one ownership scope."""
    result: list[tuple[Path, str]] = []
    for step in steps:
        source = (
            inputs_build.root_source(step["source"])
            if platform_owned
            else inputs_build.target_source(target, step["source"])
        )
        result.append((inputs_build.require_file(source), step["destination"]))
    return result


def extract_vendor(archive: Path, prefix: str, files: list[str], output: Path) -> None:
    """Project the declared bootstrap vendor closure from a pinned archive."""
    with tarfile.open(archive, "r:gz") as source:
        for relative in files:
            member = source.getmember(prefix + relative)
            stream = source.extractfile(member)
            if stream is None or not member.isfile():
                fail(f"invalid bootstrap vendor member: {relative}")
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(stream.read())

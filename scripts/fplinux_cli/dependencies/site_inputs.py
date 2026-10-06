# SPDX-License-Identifier: GPL-2.0-only
"""Select exact documentation wheels for the supported site build environment."""

from __future__ import annotations

import json
import re
import shlex
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING, Any
from urllib.parse import unquote, urlsplit

from fplinux_cli.common import (
    canonical_json_bytes,
    fail,
    read_json_object,
    replace_file_atomically,
)

from .inputs import DependencyInput

if TYPE_CHECKING:
    from collections.abc import Sequence


@dataclass(frozen=True)
class _Requirement:
    name: str
    version: str
    hashes: tuple[str, ...]


def _package_name(value: str) -> str:
    return re.sub(r"[-_.]+", "-", value).lower()


def _requirements(root: Path) -> list[_Requirement]:
    """Read the maintained, fully pinned requirements without interpreting comments."""
    path = root / "site/requirements.txt"
    declarations: dict[str, _Requirement] = {}
    continued = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        content = line.split("#", 1)[0].strip()
        if not content:
            continue
        if content.endswith("\\"):
            continued += content[:-1] + " "
            continue
        words = shlex.split(continued + content)
        continued = ""
        pin = re.fullmatch(r"([A-Za-z0-9][A-Za-z0-9._-]*)==([A-Za-z0-9.+]+)", words[0])
        if pin is None:
            fail("documentation requirements must use exact name==version pins")
        name, version = pin.groups()
        name = _package_name(name)
        hashes: set[str] = set()
        for word in words[1:]:
            checksum = re.fullmatch(r"--hash=sha256:([0-9a-f]{64})", word)
            if checksum is None:
                fail(f"documentation requirement has an unsupported option: {name}=={version}")
            hashes.add(checksum[1])
        if not hashes or name in declarations:
            fail(f"documentation requirement needs a unique pin and SHA-256 hashes: {name}")
        declarations[name] = _Requirement(name, version, tuple(sorted(hashes)))
    if continued or not declarations:
        fail("documentation requirements are empty or have an unfinished continuation")
    return [declarations[name] for name in sorted(declarations)]


def _consumer(root: Path) -> dict[str, str]:
    """Bind the wheel selection to the current documentation build job."""
    workflow = (root / ".github/workflows/pages.yml").read_text(encoding="utf-8")
    build = re.search(r"^  build:\s*(?:#.*)?$", workflow, re.MULTILINE)
    if build is None:
        fail("documentation workflow has no supported build job")
    remaining = workflow[build.end() :]
    next_job = re.search(r"^  \S", remaining, re.MULTILINE)
    block = remaining[: next_job.start()] if next_job is not None else remaining
    values: dict[str, str] = {}
    for field in ("runs-on", "python-version"):
        matches = re.findall(rf"^\s+{field}:\s*(.+)$", block, re.MULTILINE)
        if len(matches) != 1:
            fail(f"documentation workflow needs one {field} value in its build job")
        words = shlex.split(matches[0], comments=True)
        if len(words) != 1:
            fail(f"documentation workflow has an unsupported {field} value")
        values[field] = words[0]
    if values != {"runs-on": "ubuntu-24.04", "python-version": "3.14"}:
        fail("documentation wheel preservation supports Ubuntu 24.04 with Python 3.14")
    return {
        "runner": values["runs-on"],
        "implementation": "cpython",
        "python": values["python-version"],
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
        "libc_version": "2.39",
    }


def site_dependency_context(root: Path) -> dict[str, object]:
    """Return meaningful site pins, allowed hashes and the supported consumer."""
    return {
        "consumer": _consumer(root),
        "requirements": [
            {
                "name": requirement.name,
                "version": requirement.version,
                "hashes": list(requirement.hashes),
            }
            for requirement in _requirements(root)
        ],
    }


def _platform_rank(tag: str) -> int | None:
    if tag == "any":
        return 100
    aliases = {"manylinux1_x86_64": 5, "manylinux2010_x86_64": 12, "manylinux2014_x86_64": 17}
    minor = aliases.get(tag)
    if minor is None:
        match = re.fullmatch(r"manylinux_2_(\d+)_x86_64", tag)
        if match is None:
            return None
        minor = int(match[1])
    return 39 - minor if 5 <= minor <= 39 else None


def _interpreter_rank(python: str, abi: str) -> int | None:
    if python == "cp314" and abi == "cp314":
        return 0
    cpython = re.fullmatch(r"cp3(\d+)", python)
    if cpython is not None and abi == "abi3" and 2 <= int(cpython[1]) <= 14:
        return 15 - int(cpython[1])
    if python == "cp314" and abi == "none":
        return 20
    if abi != "none":
        return None
    if python == "py3":
        return 50
    generic = re.fullmatch(r"py3(\d+)", python)
    return 40 - int(generic[1]) if generic is not None and 0 <= int(generic[1]) <= 14 else None


def _wheel_rank(filename: str, requirement: _Requirement) -> tuple[int, int] | None:
    if not filename.endswith(".whl"):
        return None
    components = filename.removesuffix(".whl").split("-")
    if len(components) not in {5, 6}:
        return None
    if _package_name(components[0]) != requirement.name or components[1] != requirement.version:
        return None
    if len(components) == 6 and re.fullmatch(r"\d[A-Za-z0-9_]*", components[2]) is None:
        return None
    python_tags, abi_tags, platform_tags = components[-3:]
    ranks: list[tuple[int, int]] = []
    for python in python_tags.split("."):
        for abi in abi_tags.split("."):
            interpreter = _interpreter_rank(python, abi)
            if interpreter is None:
                continue
            for platform in platform_tags.split("."):
                rank = _platform_rank(platform)
                if rank is not None and (platform != "any" or abi == "none"):
                    ranks.append((interpreter, rank))
    return min(ranks) if ranks else None


def _python_supported(specification: str) -> bool:
    """Handle the numeric Python bounds used by the pinned published packages."""
    for clause in specification.split(","):
        if not clause.strip():
            continue
        match = re.fullmatch(r"\s*(>=|<=|==|!=|>|<)(\d+)\.(\d+)(\.\*)?\s*", clause)
        if match is None:
            fail(f"documentation wheel has an unsupported Python requirement: {specification}")
        operator, major, minor, wildcard = match.groups()
        if wildcard and operator not in {"==", "!="}:
            fail(f"documentation wheel has an unsupported Python requirement: {specification}")
        bound = (int(major), int(minor))
        python = (3, 14)
        matches = {
            ">=": python >= bound,
            "<=": python <= bound,
            "==": python == bound,
            "!=": python != bound,
            ">": python > bound,
            "<": python < bound,
        }
        if not matches[operator]:
            return False
    return True


def _wheel_url(url: str, filename: str) -> bool:
    parsed = urlsplit(url)
    return (
        parsed.scheme == "https"
        and parsed.netloc == "files.pythonhosted.org"
        and not parsed.query
        and not parsed.fragment
        and unquote(PurePosixPath(parsed.path).name) == filename
    )


def _validate_selection(
    requirements: Sequence[_Requirement], inputs: Sequence[DependencyInput]
) -> list[DependencyInput]:
    remaining = {requirement.name: requirement for requirement in requirements}
    for item in inputs:
        filename = PurePosixPath(item.destination).name
        name = _package_name(filename.split("-", 1)[0])
        requirement = remaining.get(name)
        if requirement is None or _wheel_rank(filename, requirement) is None:
            fail(f"saved documentation wheel is outside the supported selection: {item.key}")
        if (
            item.key != f"site:{name}:{requirement.version}:{filename}"
            or item.purpose != "site-python"
            or item.arch != "x86_64"
            or item.destination != f"downloads/site/{filename}"
            or item.algorithm != "sha256"
            or item.sha256 not in requirement.hashes
            or item.checksum not in {None, item.sha256}
            or item.size is None
            or not _wheel_url(item.url, filename)
        ):
            fail(f"saved documentation wheel does not match its exact declaration: {item.key}")
        del remaining[name]
    if remaining:
        names = ", ".join(f"{item.name}=={item.version}" for item in remaining.values())
        fail(f"saved documentation wheel selection is incomplete: {names}")
    return sorted(inputs, key=lambda item: item.key)


def _cached_selection(
    path: Path, context: dict[str, object], requirements: Sequence[_Requirement]
) -> list[DependencyInput] | None:
    receipt = read_json_object(path)
    if receipt is None or receipt.get("context") != context:
        return None
    records = receipt.get("inputs")
    if not isinstance(records, list) or any(not isinstance(item, dict) for item in records):
        return None
    try:
        return _validate_selection(requirements, [DependencyInput(**record) for record in records])
    except SystemExit, TypeError:
        return None


def _published_wheel(requirement: _Requirement) -> DependencyInput:
    url = f"https://pypi.org/pypi/{requirement.name}/{requirement.version}/json"
    request = urllib.request.Request(url, headers={"User-Agent": "FPLinux"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:  # noqa: S310
            metadata: Any = json.load(response)
    except (OSError, ValueError) as error:
        fail(
            "cannot resolve exact documentation requirement "
            f"{requirement.name}=={requirement.version}: {error}"
        )
    if not isinstance(metadata, dict) or not isinstance(metadata.get("urls"), list):
        fail(
            f"invalid published documentation metadata: {requirement.name}=={requirement.version}"
        )
    candidates: list[tuple[tuple[int, int], str, DependencyInput]] = []
    for record in metadata["urls"]:
        if (
            not isinstance(record, dict)
            or record.get("packagetype") != "bdist_wheel"
            or record.get("yanked")
        ):
            continue
        filename = record.get("filename")
        digests = record.get("digests")
        digest = digests.get("sha256") if isinstance(digests, dict) else None
        size = record.get("size")
        original_url = record.get("url")
        if (
            not isinstance(filename, str)
            or digest not in requirement.hashes
            or type(size) is not int
            or size <= 0
            or not isinstance(original_url, str)
            or not _wheel_url(original_url, filename)
        ):
            continue
        rank = _wheel_rank(filename, requirement)
        python_bound = record.get("requires_python") or ""
        if (
            rank is None
            or not isinstance(python_bound, str)
            or not _python_supported(python_bound)
        ):
            continue
        item = DependencyInput(
            key=f"site:{requirement.name}:{requirement.version}:{filename}",
            url=original_url,
            sha256=digest,
            size=size,
            destination=f"downloads/site/{filename}",
            purpose="site-python",
            arch="x86_64",
        )
        candidates.append((rank, filename, item))
    if not candidates:
        hashes = ", ".join(requirement.hashes)
        fail(
            f"no supported documentation wheel for {requirement.name}=={requirement.version}; "
            f"allowed SHA-256: {hashes}"
        )
    return min(candidates, key=lambda candidate: candidate[:2])[2]


def remember_site_inputs(root: Path, inputs: Sequence[DependencyInput]) -> None:
    """Keep a verified snapshot's selected originals available to offline creation."""
    context = site_dependency_context(root)
    selected = _validate_selection(_requirements(root), inputs)
    destination = root / ".cache/downloads/site/selection.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    receipt = {"context": context, "inputs": [asdict(item) for item in selected]}
    replace_file_atomically(destination, canonical_json_bytes(receipt), 0o600)


def resolve_site_inputs(
    root: Path,
    *,
    offline: bool,
    saved_inputs: Sequence[DependencyInput] | None = None,
    source_directories: Sequence[Path] = (),
) -> list[DependencyInput]:
    """Resolve current wheels, or verify saved selections without querying an index."""
    context = site_dependency_context(root)
    requirements = _requirements(root)
    if saved_inputs is not None:
        return _validate_selection(requirements, saved_inputs)
    cache_receipt = root / ".cache/downloads/site/selection.json"
    receipts = [cache_receipt]
    for source in source_directories:
        if source.is_dir():
            receipts.extend(sorted(source.rglob("selection.json")))
        elif source.name == "selection.json":
            receipts.append(source)
    for path in receipts:
        cached = _cached_selection(path, context, requirements)
        if cached is not None:
            return cached
    if offline:
        requirement = requirements[0]
        hashes = ", ".join(requirement.hashes)
        fail(
            f"documentation wheel selection is missing offline: "
            f"{requirement.name}=={requirement.version}; allowed SHA-256: {hashes}; "
            "supply its saved selection.json or dependency snapshot"
        )
    inputs = [_published_wheel(requirement) for requirement in requirements]
    remember_site_inputs(root, inputs)
    return inputs

# SPDX-License-Identifier: GPL-2.0-only
"""Materialize explicit source references into native local Alpine recipes."""

from __future__ import annotations

import fnmatch
import hashlib
import json
import re
import shutil
from dataclasses import asdict
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import fail
from fplinux_cli.manifests.source_catalog import ResolvedSource, resolve_source

from .selection import aport_producer

if TYPE_CHECKING:
    from pathlib import Path

_REFERENCE = re.compile(r"@(source|version|archive_prefix):([a-z0-9][a-z0-9_-]*)@")
_ASSIGNMENT = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)=(\"(?:[^\"\\]|\\.)*\"|'[^']*'|[^\n]*)", re.MULTILINE
)
_VARIABLE = re.compile(r"\$\{([^{}]+)\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def _template(root: Path, package: str) -> str:
    path = root / "alpine/aports" / aport_producer(package) / "APKBUILD"
    if path.is_symlink() or not path.is_file():
        fail(f"canonical Alpine recipe is missing or invalid: {path}")
    return path.read_text(encoding="utf-8")


def aport_source_ids(template: str) -> tuple[str, ...]:
    """Select the source and version records explicitly referenced by a recipe."""
    return tuple(dict.fromkeys(match[2] for match in _REFERENCE.finditer(template)))


def _reference_value(source: ResolvedSource, kind: str) -> str:
    if kind == "source":
        return source.name
    if kind == "version":
        return source.version
    if source.archive_prefix is None:
        fail(f"source archive prefix is missing: {source.id}")
    return source.archive_prefix


def aport_sources(root: Path, package: str) -> tuple[ResolvedSource, ...]:
    """Resolve original references in their declared native source order."""
    aport_declaration(root, package)
    return tuple(
        resolve_source(root, match[2])
        for match in _REFERENCE.finditer(_template(root, package))
        if match[1] == "source"
    )


def _expand(value: str, variables: dict[str, str], *, package: str) -> str:
    """Read the literal substitutions used by maintained top-level declarations."""
    if "$(" in value or "`" in value:
        fail(f"{package} dependency declaration uses an unsupported command substitution")

    def replace(match: re.Match[str]) -> str:
        expression = match[1] or match[2]
        parsed = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)(.*)", expression)
        if parsed is None:
            fail(f"{package} declaration has an unsupported variable: {expression}")
        name, operation = parsed.groups()
        if name not in variables:
            fail(f"{package} declaration has an unknown variable: {name}")
        expanded = _expand(
            variables[name], {k: v for k, v in variables.items() if k != name}, package=package
        )
        if not operation:
            return expanded
        if operation.startswith("/"):
            parts = operation.split("/")
            if len(parts) == 3 and parts[1]:
                return expanded.replace(parts[1], parts[2], 1)
        for operator in ("%%", "%", "##", "#"):
            if not operation.startswith(operator):
                continue
            positions = range(len(expanded) + 1)
            indices = reversed(positions) if operator in {"%", "##"} else positions
            for index in indices:
                candidate = expanded[index:] if operator.startswith("%") else expanded[:index]
                if fnmatch.fnmatchcase(candidate, operation.removeprefix(operator)):
                    return expanded[:index] if operator.startswith("%") else expanded[index:]
            return expanded
        return fail(f"{package} declaration has an unsupported substitution: {expression}")

    result = _VARIABLE.sub(replace, value)
    if "$" in result:
        fail(f"{package} declaration contains unresolved shell syntax")
    return result


def aport_declaration(root: Path, package: str) -> dict[str, Any]:
    """Bind the package template to only the source records it actually consumes."""
    text = _template(root, package)
    selected = {source_id: resolve_source(root, source_id) for source_id in aport_source_ids(text)}
    native = _REFERENCE.sub(lambda match: _reference_value(selected[match[2]], match[1]), text)
    if "@source:" in native or "@version:" in native:
        fail(f"unresolved Alpine source reference: {package}")
    variables = {}
    for match in _ASSIGNMENT.finditer(native.replace("\\\n", "")):
        value = match[2]
        variables[match[1]] = (
            value[1:-1] if value.startswith(("'", '"')) else value.split(" #", 1)[0].rstrip()
        )
    context: dict[str, Any] = {
        "sources": _expand(variables.get("source", ""), variables, package=package).split(),
        "originals": {key: asdict(source) for key, source in selected.items()},
    }
    if any("://" in source for source in context["sources"]):
        fail(f"{package} external sources must use source references")
    for name in (
        "pkgname",
        "pkgver",
        "pkgrel",
        "arch",
        "provides",
        "provider_priority",
        "replaces",
        "depends",
        "makedepends_build",
        "makedepends_host",
        "subpackages",
    ):
        value = _expand(variables.get(name, ""), variables, package=package)
        context[name] = value if name in {"pkgname", "pkgver", "pkgrel"} else sorted(value.split())
    return context


def font_source_manifest(root: Path) -> dict[str, Any]:
    """Bind the standalone font compiler to selected originals and the npm owner."""
    npm = json.loads((root / "package-lock.json").read_text())
    return {
        "converter_version": npm["packages"]["node_modules/lv_font_conv"]["version"],
        "sources": {source.name: asdict(source) for source in aport_sources(root, "fplinux-ui")},
    }


def checksum_block(data: bytes, *, path: Path) -> tuple[int, int, dict[str, str]] | None:
    """Read the canonical literal SHA-512 block without sourcing shell code."""
    offset = 0
    start: int | None = None
    entries: dict[str, str] = {}
    for line in data.splitlines(keepends=True):
        value = line.rstrip(b"\r\n")
        if start is None:
            if value == b'sha512sums=""':
                return offset, offset + len(line), {}
            if value == b'sha512sums="':
                start = offset
        elif value == b'"':
            return start, offset + len(line), entries
        elif value:
            words = value.decode().split()
            if len(words) != 2 or re.fullmatch(r"[0-9a-f]{128}", words[0]) is None:
                fail(f"invalid local source checksum in {path}")
            digest, name = words
            if name in entries or "/" in name or name in {".", ".."}:
                fail(f"invalid or duplicate local source checksum in {path}: {name}")
            entries[name] = digest
        offset += len(line)
    if start is not None:
        fail(f"unterminated sha512sums block: {path}")
    return None


def replace_checksum_block(data: bytes, checksums: dict[str, str], *, path: Path) -> bytes:
    """Replace the one literal native checksum block and preserve its surrounding bytes."""
    block = (
        b'sha512sums="\n'
        + b"".join(f"{digest}  {name}\n".encode() for name, digest in checksums.items())
        + b'"\n'
    )
    bounds = checksum_block(data, path=path)
    if bounds is None:
        return data + (b"" if data.endswith(b"\n") else b"\n") + block
    return data[: bounds[0]] + block + data[bounds[1] :]


def materialize_source_metadata(
    package: str,
    root: Path,
    directory: Path,
    *,
    cache: Path,
    verify_local: bool = True,
) -> None:
    """Verify originals, copy local inputs and emit a native APKBUILD in one stage."""
    apkbuild = directory / "APKBUILD"
    canonical = apkbuild.read_bytes()
    checksum_metadata = checksum_block(canonical, path=apkbuild)
    local_checksums = checksum_metadata[2] if checksum_metadata is not None else {}
    resolved = aport_sources(root, package)
    external_names = {source.name for source in resolved}
    source_names = aport_declaration(root, package)["sources"]
    local_names = set(source_names) - external_names
    if not resolved and not local_names and checksum_metadata is None:
        return
    if verify_local:
        if local_names != local_checksums.keys():
            fail(f"local Alpine source checksum inventory differs from its recipe: {package}")
        for name, expected in local_checksums.items():
            local_source = directory / name
            if local_source.is_symlink() or not local_source.is_file():
                fail(f"local Alpine source is missing or invalid: {local_source}")
            with local_source.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha512").hexdigest()
            if actual != expected:
                fail(f"local Alpine source SHA-512 mismatch: {package}: {name}")
    external_checksums = {}
    for source in resolved:
        original = source.acquire(cache, offline=True)
        destination = directory / source.name
        if destination.exists() or destination.is_symlink():
            fail(f"Alpine original conflicts with a local source: {destination}")
        shutil.copyfile(original, destination)
        with original.open("rb") as stream:
            external_checksums[source.name] = hashlib.file_digest(stream, "sha512").hexdigest()

    def expand(match: re.Match[str]) -> str:
        source = resolve_source(root, match[2])
        return _reference_value(source, match[1])

    native = _REFERENCE.sub(expand, canonical.decode()).encode()
    if b"@source:" in native or b"@version:" in native:
        fail(f"unresolved Alpine source reference: {package}")
    checksums = {**external_checksums, **local_checksums}
    # Checksum regeneration admits newly declared local files before abuild pins them.
    ordered_checksums = {name: checksums[name] for name in source_names if name in checksums}
    if aport_producer(package) == "fplinux-ui":
        manifest_path = directory / "fplinux-font-sources.json"
        manifest = (json.dumps(font_source_manifest(root)) + "\n").encode()
        manifest_path.write_bytes(manifest)
        native = native.replace(b'source="\n', b'source="\n\tfplinux-font-sources.json\n', 1)
        ordered_checksums = {
            manifest_path.name: hashlib.sha512(manifest).hexdigest(),
            **ordered_checksums,
        }
    apkbuild.write_bytes(replace_checksum_block(native, ordered_checksums, path=apkbuild))

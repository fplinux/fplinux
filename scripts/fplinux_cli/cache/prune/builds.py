# SPDX-License-Identifier: GPL-2.0-only
"""Retain shared Linux sources and currently declared host tools."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, load_toml, read_json_object
from fplinux_cli.manifests.paths import discover_platforms
from fplinux_cli.manifests.values import nonempty_string, sha256_value

from .model import InventoryEntry
from .paths import SOURCE_SHA256, WORKSPACE_NAMESPACES, tree_size

if TYPE_CHECKING:
    from pathlib import Path


def _current_host_tools() -> frozenset[str] | None:
    """Retain tools declared by any platform without reading target-owned inputs."""
    try:
        names: set[str] = set()
        for platform in discover_platforms():
            config = load_toml(ROOT / "platforms" / platform / "platform.toml")
            for tool in config["host"]["tools"]:
                names.add(nonempty_string(tool["name"], "host tool name"))
    except KeyError, OSError, TypeError, ValueError, SystemExit:
        return None
    return frozenset(names)


def host_tool_entries(cache: Path) -> list[InventoryEntry]:
    """Protect active tool slots and unknown data; retire only recognized tool receipts."""
    root = cache / "host-tools"
    if not root.is_dir() or root.is_symlink():
        return []
    current = _current_host_tools()
    entries: list[InventoryEntry] = []
    for path in sorted(root.iterdir()):
        disposable = False
        if not path.is_dir() or path.is_symlink():
            reason = "not a managed host tool cache directory"
        elif current is None:
            reason = "current host tool declarations are unavailable"
        elif path.name in current:
            reason = "current host tool cache"
        else:
            receipt = path / "receipt.json"
            record = None if receipt.is_symlink() else read_json_object(receipt)
            owned = (
                record is not None
                and set(record) == {"recipe", "sha256"}
                and all(
                    isinstance(value, str) and SOURCE_SHA256.fullmatch(value)
                    for value in record.values()
                )
            )
            disposable = bool(owned)
            reason = "retired host tool cache" if owned else "unrecognized host tool cache"
        logical, allocated = tree_size(path) if disposable else (None, None)
        entries.append(
            InventoryEntry(
                f"host-tools/{path.name}",
                "candidate" if disposable else "protected",
                reason,
                logical,
                allocated,
            )
        )
    return entries


def _current_linux_sources() -> frozenset[str] | None:
    """Read pinned Linux archives without requiring unrelated firmware or build inputs."""
    try:
        sources = load_toml(ROOT / "sources.lock.toml")
        current: set[str] = set()
        for platform in discover_platforms():
            config = load_toml(ROOT / "platforms" / platform / "platform.toml")
            source_lock = nonempty_string(
                config["linux"]["source_lock"], f"platform {platform} Linux source lock"
            )
            current.add(sha256_value(sources[source_lock]["sha256"], source_lock))
    except KeyError, OSError, TypeError, ValueError, SystemExit:
        return None
    return frozenset(current)


def _linux_source_marker_matches(path: Path) -> bool:
    """Recognize a managed archive slot before considering it disposable."""
    marker = path / ".fplinux-base"
    if marker.is_symlink() or not marker.is_file():
        return False
    try:
        return marker.read_text().strip() == path.name
    except OSError, UnicodeDecodeError:
        return False


def linux_cache_entries(cache: Path) -> list[InventoryEntry]:
    """Retain current shared Linux sources and remove managed retired or staging slots."""
    linux = cache / "linux"
    if not linux.is_dir() or linux.is_symlink():
        return []
    current = _current_linux_sources()
    entries: list[InventoryEntry] = []
    for namespace in ("sources", "originals", "staging"):
        root = linux / namespace
        if not root.is_dir() or root.is_symlink():
            continue
        for path in sorted(root.iterdir(), key=lambda item: item.name):
            identity = f"linux/{namespace}/{path.name}"
            disposable = False
            if (
                path.is_symlink()
                or not path.is_dir()
                or SOURCE_SHA256.fullmatch(path.name) is None
                or (namespace != "staging" and not _linux_source_marker_matches(path))
            ):
                reason = "not a managed Linux source cache directory"
            elif namespace == "staging":
                reason = "disposable prepared Linux staging slot"
                disposable = True
            elif current is None:
                reason = "current Linux source declarations are unavailable"
            elif path.name in current:
                reason = "current shared Linux source archive"
            else:
                reason = "retired shared Linux source archive"
                disposable = True
            logical, allocated = tree_size(path) if disposable else (None, None)
            entries.append(
                InventoryEntry(
                    identity,
                    "candidate" if disposable else "protected",
                    reason,
                    logical,
                    allocated,
                )
            )
    return entries


def workspace_entries(cache: Path) -> list[InventoryEntry]:
    """Classify every real staged workspace as disposable command state."""
    entries: list[InventoryEntry] = []
    for namespace in sorted(WORKSPACE_NAMESPACES):
        root = cache / namespace
        if not root.is_dir() or root.is_symlink():
            continue
        for path in sorted(root.iterdir(), key=lambda item: item.name):
            if not path.is_dir() or path.is_symlink():
                entries.append(
                    InventoryEntry(
                        f"{namespace}/{path.name}",
                        "protected",
                        "not a disposable workspace directory",
                        None,
                        None,
                    )
                )
                continue
            logical, allocated = tree_size(path)
            entries.append(
                InventoryEntry(
                    f"{namespace}/{path.name}",
                    "candidate",
                    "disposable staged workspace",
                    logical,
                    allocated,
                )
            )
    return entries

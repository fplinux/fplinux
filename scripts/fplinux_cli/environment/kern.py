# SPDX-License-Identifier: GPL-2.0-only
"""Install and invoke the pinned project-local Kern runtime."""

from __future__ import annotations

import os
import re
import secrets
import shutil
import tarfile
import tempfile
from pathlib import Path
from typing import Any

from fplinux_cli.common import ROOT, fail, sha256_file

from .downloads import download_locked_file
from .images import load_container_lock

KERN_PROBE_TIMEOUT = 60


def ensure_project_directory(path: Path) -> Path:
    """Create one exact project-owned directory without following a symlink."""
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        fail(f"invalid project runtime directory: {path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    return path


def kern_environment() -> dict[str, str]:
    """Confine persistent Kern state while retaining the host runtime directory."""
    root = ensure_project_directory(ROOT / ".cache/kern")
    environment = os.environ.copy()
    for variable, name in (
        ("XDG_CACHE_HOME", "cache"),
        ("XDG_DATA_HOME", "data"),
        ("XDG_CONFIG_HOME", "config"),
    ):
        environment[variable] = str(ensure_project_directory(root / name))
    return environment


def _kern_path() -> Path:
    return ROOT / ".cache/tools/kern/kern"


def kern_available(lock: dict[str, Any] | None = None) -> bool:
    """Return whether the exact pinned Kern binary is already project-local."""
    if lock is None:
        lock = load_container_lock()
    executable = _kern_path()
    return (
        not executable.is_symlink()
        and executable.is_file()
        and sha256_file(executable) == lock["kern"]["binary_sha256"]
    )


def require_kern(lock: dict[str, Any] | None = None) -> str:
    """Return the exact project-local Kern binary."""
    if lock is None:
        lock = load_container_lock()
    if not kern_available(lock):
        fail("Kern is not ready for this checkout; run ./fplinux setup online first")
    return str(_kern_path())


def install_kern(lock: dict[str, Any], *, offline: bool = False) -> str:
    """Install the pinned static Kern binary under the project cache."""
    if kern_available(lock):
        return str(_kern_path())
    kern_lock = lock["kern"]
    archive = download_locked_file(
        kern_lock["archive_url"],
        kern_lock["archive_sha256"],
        ROOT / ".cache/downloads/kern/kern.tar.gz",
        offline=offline,
    )
    destination = _kern_path()
    ensure_project_directory(destination.parent)
    temporary: Path | None = None
    try:
        with tarfile.open(archive, "r:gz") as bundle:
            try:
                member = bundle.getmember("kern")
            except KeyError:
                fail("pinned Kern archive contains no kern binary")
            if not member.isfile():
                fail("pinned Kern archive kern entry is not a regular file")
            source = bundle.extractfile(member)
            if source is None:
                fail("could not read kern from the pinned release archive")
            with tempfile.NamedTemporaryFile(
                dir=destination.parent,
                prefix=".kern.",
                delete=False,
            ) as output:
                temporary = Path(output.name)
                shutil.copyfileobj(source, output)
        actual = sha256_file(temporary)
        if actual != kern_lock["binary_sha256"]:
            fail(
                "Kern binary SHA256 mismatch: "
                f"expected {kern_lock['binary_sha256']}, received {actual}"
            )
        temporary.chmod(0o755)
        temporary.replace(destination)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return str(destination)


def kern_box_name(label: str) -> str:
    """Return one collision-resistant foreground box name owned by this invocation."""
    normalized = re.sub(r"[^a-z0-9-]+", "-", label.lower()).strip("-") or "task"
    return f"fplinux-{normalized}-{os.getpid()}-{secrets.token_hex(3)}"

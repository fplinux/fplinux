# SPDX-License-Identifier: GPL-2.0-only
"""Prepare disposable, pinned Python dependencies for host namespace tests."""

from __future__ import annotations

import os
import re
import sys
import tarfile
import tempfile
import venv
import zipfile
from contextlib import contextmanager
from pathlib import Path, PurePosixPath
from typing import TYPE_CHECKING

from fplinux_cli.common import ROOT, fail
from fplinux_cli.dependencies.inputs import environment_inputs
from fplinux_cli.environment.downloads import download_locked_file

if TYPE_CHECKING:
    from collections.abc import Iterator

    from fplinux_cli.reporting.run import RunReporter


def _extract_packaging(archive: Path, destination: Path) -> None:
    """Use the pure Python files from the same locked package as the quality image."""
    prefix = PurePosixPath("usr/lib/python3.14/site-packages")
    with tarfile.open(archive, "r:gz") as package:
        for member in package:
            path = PurePosixPath(member.name)
            if not member.isfile() or not path.is_relative_to(prefix):
                continue
            relative = path.relative_to(prefix)
            if relative.suffix not in {".py", ".typed"} and not any(
                part.endswith(".dist-info") for part in relative.parts
            ):
                continue
            source = package.extractfile(member)
            if source is None:
                fail(f"cannot read locked Python package member: {member.name}")
            output = destination / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            with source:
                output.write_bytes(source.read())


@contextmanager
def host_pytest_environment(
    workspace: Path, reporter: RunReporter
) -> Iterator[tuple[Path, dict[str, str]]]:
    """Install declared pure Python inputs without pip or system-site packages."""
    if sys.version_info[:2] != (3, 14):
        fail("host namespace tests require Python 3.14")
    with reporter.stage("host-namespace") as stage:
        stage.run(["unshare", "--map-auto", "--map-root-user", "--", "true"], timeout=30)
    with tempfile.TemporaryDirectory(
        prefix="fplinux-host-python-",
        dir="/tmp",  # Mapped users cannot traverse private checkout ancestors.
    ) as temporary:
        root = Path(temporary)
        with reporter.stage("host-python"):
            venv.EnvBuilder(with_pip=False, symlinks=True).create(root)
            library = root / "lib/python3.14/site-packages"
            library.mkdir(parents=True, exist_ok=True)
            selected = [
                item
                for item in environment_inputs(workspace)
                if item.key
                in {
                    "container:pytest",
                    "container:iniconfig",
                    "container:pluggy",
                    "container:pygments",
                }
                or re.fullmatch(r"py3-packaging-\d.*\.apk", Path(item.destination).name)
            ]
            if len(selected) != 5:
                fail("host pytest requires the four declared wheels and locked packaging package")
            for item in selected:
                archive = download_locked_file(
                    item.url,
                    item.checksum or item.sha256 or "",
                    ROOT / ".cache" / item.destination,
                    algorithm=item.algorithm,
                    size=item.size,
                )
                if archive.suffix == ".whl":
                    with zipfile.ZipFile(archive) as wheel:
                        wheel.extractall(library)  # noqa: S202 -- exact declared archive checksums.
                else:
                    _extract_packaging(archive, library)
            # Only this disposable directory contains the public pinned dependencies.
            root.chmod(0o755)
            for path in root.rglob("*"):
                if path.is_symlink():
                    continue
                mode = 0o755 if path.is_dir() else (path.stat().st_mode & 0o777) | 0o444
                path.chmod(mode)
        environment = os.environ.copy()
        environment.pop("PYTHONHOME", None)
        environment.pop("PYTEST_ADDOPTS", None)
        environment.pop("PYTEST_PLUGINS", None)
        environment.update(
            {
                "HOME": str(root),
                "PATH": str(root / "bin") + os.pathsep + environment.get("PATH", os.defpath),
                "PYTHONPATH": str(workspace / "scripts"),
                "PYTHONNOUSERSITE": "1",
                "PYTHONDONTWRITEBYTECODE": "1",
                "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1",
            }
        )
        yield root / "bin/python", environment

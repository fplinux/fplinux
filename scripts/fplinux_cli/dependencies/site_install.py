# SPDX-License-Identifier: GPL-2.0-only
"""Install documentation tooling exclusively from preserved originals."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes, sha256_file
from fplinux_cli.data_paths import DATA_ENV
from fplinux_cli.environment.node_runtime import install_node_runtime, populate_npm_cache

from .inputs import source_input
from .site_inputs import site_runtime_sources

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

    from .inputs import DependencyInput
    from .site_inputs import SiteConsumer


def site_runtime(root: Path, cache: Path) -> Path:
    """Resolve the runtime owned by the current Node and npm declarations."""
    node, npm = site_runtime_sources(root)
    return _runtime_path(root, cache, source_input(node), source_input(npm))


def _runtime_path(root: Path, cache: Path, node: DependencyInput, npm: DependencyInput) -> Path:
    environment = root / "scripts/fplinux_cli/environment"
    recipe = sha256_bytes(
        canonical_json_bytes(
            {
                "node": (node.algorithm, node.checksum or node.sha256),
                "npm": (npm.algorithm, npm.checksum or npm.sha256),
                "installation": sha256_file(environment / "node_runtime.py"),
                "extraction": sha256_file(environment / "archives.py"),
            }
        )
    )
    return cache / "site/runtime" / recipe


def install_site(
    root: Path,
    cache: Path,
    inputs: Sequence[DependencyInput],
    originals: Mapping[str, Path],
    consumer: SiteConsumer,
) -> None:
    """Install pinned tooling and the native site lock from already admitted originals."""
    by_key = {item.key: item for item in inputs}
    runtime = _runtime_path(root, cache, by_key["source:site-node"], by_key["source:npm"])
    runtime.parent.mkdir(parents=True, exist_ok=True)
    if not runtime.exists():
        with tempfile.TemporaryDirectory(dir=runtime.parent, prefix=".node-") as temporary:
            prepared = Path(temporary) / "runtime"
            install_node_runtime(originals["source:site-node"], originals["source:npm"], prepared)
            prepared.rename(runtime)
    environment = {
        **os.environ,
        DATA_ENV: str(cache),
        "PATH": str(runtime / "bin") + os.pathsep + os.environ.get("PATH", ""),
    }
    node_command, npm_command = runtime / "bin/node", runtime / "bin/npm"
    for command, expected in (
        (node_command, f"v{consumer['node']}"),
        (npm_command, consumer["npm"]),
    ):
        result = subprocess.run(
            [str(command), "--version"],
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode or result.stdout.strip() != expected:
            fail(f"preserved documentation runtime has an unexpected version: {command.name}")
    npm_cache = cache / "site/npm-cache"
    archives = [originals[item.key] for item in inputs if item.purpose == "site-npm-package"]
    populate_npm_cache(npm_command, npm_cache, archives)
    installed = subprocess.run(
        [
            str(npm_command),
            "ci",
            "--offline",
            "--ignore-scripts",
            "--strict-peer-deps",
            "--no-audit",
            "--no-fund",
            "--cache",
            str(npm_cache),
        ],
        cwd=root / "site",
        env=environment,
        check=False,
    )
    if installed.returncode:
        fail(f"documentation dependency installation failed (exit {installed.returncode})")
    with tempfile.TemporaryDirectory(dir=cache / "site", prefix=".node-link-") as temporary:
        link = Path(temporary) / "node"
        link.symlink_to(Path("runtime") / runtime.name)
        link.replace(cache / "site/node")
    workflow_path = os.environ.get("GITHUB_PATH")
    if workflow_path:
        with Path(workflow_path).open("a", encoding="utf-8") as stream:
            stream.write(str(runtime / "bin") + "\n")
    print(f"Documentation runtime: {cache / 'site/node/bin'}")

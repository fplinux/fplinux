# SPDX-License-Identifier: GPL-2.0-only
"""Install documentation tooling exclusively from preserved originals."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes, sha256_file
from fplinux_cli.dependencies.inputs import acquire_inputs
from fplinux_cli.dependencies.site_inputs import site_dependency_selection
from fplinux_cli.environment.install import install_node_runtime, populate_npm_cache
from fplinux_cli.manifests.source_catalog import resolve_source


def install_site() -> None:
    """Prepare exact Node/npm and feed the site's native lock into an offline install."""
    root = common.ROOT
    cache = root / ".cache"
    inputs, _context = site_dependency_selection(root)
    local = acquire_inputs(inputs, cache, offline=True)
    node = resolve_source(root, "site-node")
    npm = resolve_source(root, "npm")
    recipe = sha256_bytes(
        canonical_json_bytes(
            {
                "node": node.checksum,
                "npm": npm.checksum,
                "installation": sha256_file(Path(__file__)),
                "shared_runtime": sha256_file(root / "scripts/fplinux_cli/environment/install.py"),
            }
        )
    )
    runtimes = cache / "site/runtime"
    runtimes.mkdir(parents=True, exist_ok=True)
    runtime = runtimes / recipe
    if not runtime.exists():
        with tempfile.TemporaryDirectory(dir=runtimes, prefix=".node-") as temporary:
            prepared = Path(temporary) / "runtime"
            install_node_runtime(local["source:site-node"], local["source:npm"], prepared)
            prepared.rename(runtime)
    environment = {
        **os.environ,
        "PATH": str(runtime / "bin") + os.pathsep + os.environ.get("PATH", ""),
    }
    node_command, npm_command = runtime / "bin/node", runtime / "bin/npm"
    for command, expected in ((node_command, f"v{node.version}"), (npm_command, npm.version)):
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
    archives = [local[item.key] for item in inputs if item.purpose == "site-npm-package"]
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
        link.symlink_to(Path("runtime") / recipe)
        link.replace(cache / "site/node")
    workflow_path = os.environ.get("GITHUB_PATH")
    if workflow_path:
        with Path(workflow_path).open("a", encoding="utf-8") as stream:
            stream.write(str(runtime / "bin") + "\n")
    print(f"Documentation runtime: {cache / 'site/node/bin'}")

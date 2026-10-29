# SPDX-License-Identifier: GPL-2.0-only
"""Run the site's own scripts using its prepared, pinned tools."""

from __future__ import annotations

import os

from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import DATA_ENV, data_root
from fplinux_cli.dependencies.site_install import site_runtime
from fplinux_cli.reporting.run import RunReporter


def run_site(action: str) -> None:
    """Run a whole-site operation without downloading or selecting ambient npm."""
    scripts = {
        "format": "format",
        "check": "check",
        "build": "build",
        "build-offline": "build:offline",
    }
    if action not in scripts:
        fail(f"unsupported site action: {action}")
    cache = data_root(ROOT)
    runtime = site_runtime(ROOT, cache)
    link = cache / "site/node"
    node, npm = runtime / "bin/node", runtime / "bin/npm"
    if (
        not link.is_symlink()
        or link.resolve() != runtime.resolve()
        or not node.is_file()
        or not npm.is_file()
        or not (ROOT / "site/node_modules").is_dir()
    ):
        fail("site tooling is not ready; run ./fplinux setup --site")
    env = {
        **os.environ,
        DATA_ENV: str(cache),
        "PATH": str(runtime / "bin") + os.pathsep + os.environ.get("PATH", ""),
        "NPM_CONFIG_OFFLINE": "true",
        "NPM_CONFIG_AUDIT": "false",
        "NPM_CONFIG_FUND": "false",
    }
    reporter = RunReporter.create("site", target=None, verbose=False, selected_data_root=cache)
    with reporter.stage(action, passthrough=True) as stage:
        stage.run([str(npm), "run", scripts[action]], cwd=ROOT / "site", env=env)
    reporter.finish()

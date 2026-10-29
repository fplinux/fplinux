# SPDX-License-Identifier: GPL-2.0-only
"""Inspect the exact setup inputs without changing the managed store."""

from __future__ import annotations

from fplinux_cli.common import ROOT, fail
from fplinux_cli.data_paths import data_root
from fplinux_cli.dependencies.inputs import deduplicate_inputs, environment_inputs
from fplinux_cli.environment.definition import environment_recipe_digest, load_environment_lock
from fplinux_cli.environment.downloads import locked_file_status
from fplinux_cli.environment.kern import kern_available
from fplinux_cli.environment.runtime import environment_rootfs
from fplinux_cli.environment.setup import target_setup_inputs
from fplinux_cli.environment.state import load_environment_state


def inspect_inputs(target: str | None, *, host: str, site: bool, all_inputs: bool) -> None:
    """Show every selected original and report readiness without executing its tools."""
    cache = data_root(ROOT)
    if all_inputs:
        from fplinux_cli.dependencies.workflow import checkout_declarations  # noqa: PLC0415

        inputs, _context = checkout_declarations()
        setup = "./fplinux setup --all"
    elif site:
        from fplinux_cli.dependencies.site_inputs import site_dependency_selection  # noqa: PLC0415

        inputs, _context = site_dependency_selection(ROOT)
        setup = "./fplinux setup --site"
    elif target is not None:
        inputs = deduplicate_inputs(
            [*environment_inputs(ROOT), *target_setup_inputs(target, host=host)]
        )
        setup = f"./fplinux setup {target} --host {host}"
        print("target selection includes all supported profiles and build types")
    else:
        inputs = environment_inputs(ROOT)
        setup = "./fplinux setup"

    counts = {"present": 0, "missing": 0, "mismatched": 0}
    for item in inputs:
        digest = item.checksum or item.sha256
        if digest is None:
            fail(f"dependency input has no checksum: {item.key}")
        status = locked_file_status(
            cache / item.destination, digest, algorithm=item.algorithm, size=item.size
        )
        counts[status] += 1
        if status != "present":
            print(f"{status}: {item.key}: {item.destination}")
    print(
        f"inputs: {counts['present']} present, {counts['missing']} missing, "
        f"{counts['mismatched']} mismatched"
    )
    if not site:
        lock = load_environment_lock()
        state = load_environment_state(cache, environment_recipe_digest(lock))
        available = (
            state is not None
            and environment_rootfs(cache, state).is_dir()
            and kern_available(lock, cache=cache)
        )
        if available:
            print("environment: receipt and runtime present; installed content not checked")
        else:
            print("environment: unavailable")
    if site or all_inputs:
        from fplinux_cli.dependencies.site_install import site_runtime  # noqa: PLC0415

        runtime = site_runtime(ROOT, cache)
        link = cache / "site/node"
        available = (
            link.is_symlink()
            and link.resolve() == runtime.resolve()
            and (runtime / "bin/node").is_file()
            and (runtime / "bin/npm").is_file()
            and (ROOT / "site/node_modules").is_dir()
        )
        print(
            "site tooling: present; installation not checked"
            if available
            else "site tooling: unavailable"
        )
    if counts["missing"] or counts["mismatched"]:
        print(f"prepare inputs with: {setup}")
        raise SystemExit(1)

# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and publish the exact locked Alpine build-environment rootfs."""

from __future__ import annotations

import json
import os
import secrets
import shutil
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.alpine.aports import materialize_aport_sources
from fplinux_cli.alpine.sources import font_source_manifest
from fplinux_cli.build import DEFAULT_BUILD_MEMORY_MIB
from fplinux_cli.build.host_tools.host import extract_host_members
from fplinux_cli.common import ROOT, error_message, fail
from fplinux_cli.dependencies.inputs import (
    acquire_inputs,
    environment_inputs,
    terminus_font_environment_inputs,
    ui_font_environment_inputs,
    ui_icon_environment_inputs,
)
from fplinux_cli.reporting.run import RunReporter

from .definition import (
    ENVIRONMENT_RECIPE_INPUTS,
    environment_recipe_digest,
    load_environment_lock,
    spreadtrum_flash_environment_source,
)
from .git_hooks import install_git_hooks
from .kern import ensure_project_directory, install_kern, kern_box_name, kern_environment
from .runtime import (
    current_environment_state,
    environment_arguments,
    probe_environment_state,
    publish_environment,
    remove_environment_rootfs,
)

if TYPE_CHECKING:
    from .state import EnvironmentState

_ENVIRONMENT_SETUP_TIMEOUT = 2 * 60 * 60


def _stage_environment_inputs(context: Path, *, offline: bool) -> None:
    """Capture the exact recipe and verified downloads without unrelated cache files."""
    for relative in ENVIRONMENT_RECIPE_INPUTS:
        source = ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"environment recipe input is missing or invalid: {source}")
        destination = context / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    input_directory = context / "inputs"
    input_directory.mkdir()
    ui_fonts = {item.key for item in ui_font_environment_inputs(ROOT)}
    terminal_fonts = {item.key for item in terminus_font_environment_inputs(ROOT)}
    ui_icons = {item.key for item in ui_icon_environment_inputs(ROOT)}
    inputs = environment_inputs(ROOT)
    originals = acquire_inputs(
        [item for item in inputs if item.key != "source:kern"], ROOT / ".cache", offline=offline
    )
    for item in inputs:
        if item.key == "source:kern":
            continue
        source = originals[item.key]
        if item.key == "source:spreadtrum_flash":
            projection = spreadtrum_flash_environment_source(ROOT)
            extract_host_members(
                source,
                f"spreadtrum_flash-{projection['commit']}/",
                projection["members"],
                projection["files"],
                context / "quality/spreadtrum-flash",
            )
            continue
        if item.key in ui_fonts:
            destination = context / "quality/ui-fonts" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, destination)
            continue
        if item.key in terminal_fonts:
            destination = context / "quality/terminus" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, destination)
            continue
        if item.key in ui_icons:
            destination = context / "quality/ui-icons" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, destination)
            continue
        destination = (
            context / "npm" / source.name
            if item.purpose == "npm-package"
            else input_directory / item.destination
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            os.link(source, destination)
    (context / "quality/ui-fonts/fplinux-font-sources.json").write_text(
        json.dumps(font_source_manifest(ROOT)) + "\n"
    )
    materialize_aport_sources(
        "fplinux-font-terminus", ROOT, context / "quality/terminus-aport", cache=ROOT / ".cache"
    )
    (context / "environment-inputs.json").write_text(json.dumps(load_environment_lock()) + "\n")


def _prepare_rootfs(  # noqa: PLR0913 -- captured inputs, receipt markers and reporting are distinct.
    kern: str,
    candidate: Path,
    *,
    context: Path,
    lock: dict[str, Any],
    recipe: str,
    generation: str,
    reporter: RunReporter,
) -> None:
    base = next(record for record in lock["input"] if record["purpose"] == "environment-base")
    extraction_environment = kern_environment()
    extraction_environment["PYTHONPATH"] = str(ROOT / "scripts")
    with reporter.stage("environment-base") as stage:
        stage.run(
            [
                "unshare",
                "--map-auto",
                "--map-root-user",
                "--",
                sys.executable,
                "-m",
                "fplinux_cli.environment.rootfs",
                str(context / "inputs" / base["destination"]),
                str(candidate),
            ],
            cwd=ROOT,
            env=extraction_environment,
            timeout=_ENVIRONMENT_SETUP_TIMEOUT,
        )
    packages = [
        str(Path("/inputs") / record["destination"])
        for record in lock["input"]
        if record["purpose"] == "environment-package"
    ]
    with reporter.stage("environment-install") as stage:
        stage.run(
            [
                kern,
                "box",
                kern_box_name("environment-install"),
                *environment_arguments(candidate),
                "--bind-rootfs",
                "--uid-range",
                "--memory",
                f"{DEFAULT_BUILD_MEMORY_MIB}m",
                "--network",
                "none",
                "--volume",
                f"{context}:/recipe:ro",
                "--volume",
                f"{context / 'inputs'}:/inputs:ro",
                "--quiet",
                "--",
                "sh",
                "/recipe/scripts/fplinux_cli/environment/install/prepare.sh",
                "/recipe/environment-inputs.json",
                "/inputs",
                "/recipe",
                recipe,
                generation,
                *packages,
            ],
            cwd=ROOT,
            env=kern_environment(),
            timeout=_ENVIRONMENT_SETUP_TIMEOUT,
        )


def setup(
    *,
    force: bool = False,
    offline: bool = False,
    reporter: RunReporter | None = None,
    lock: dict[str, Any] | None = None,
    environment_recipe: str | None = None,
) -> EnvironmentState:
    """Reuse an exact environment or prepare a replacement while retaining the old tree."""
    own_reporter = reporter is None
    if reporter is None:
        reporter = RunReporter.create("setup", target=None, verbose=False)
    if lock is None:
        lock = load_environment_lock()
    kern = install_kern(lock, offline=offline)
    current_recipe = environment_recipe_digest(lock)
    if environment_recipe is not None and environment_recipe != current_recipe:
        fail("environment inputs changed before setup")
    environment_recipe = current_recipe
    current = current_environment_state(kern, environment_recipe)
    if current is not None and not force:
        install_git_hooks()
        print("Build environment is ready")
        if own_reporter:
            reporter.finish()
        return current

    directory = ensure_project_directory(ROOT / ".cache/environment")
    candidate = Path(tempfile.mkdtemp(dir=directory, prefix="candidate-"))
    generation = secrets.token_hex(32)
    failed = False
    try:
        with tempfile.TemporaryDirectory(dir=directory, prefix="recipe-") as temporary:
            context = Path(temporary)
            with reporter.stage("environment-inputs"):
                _stage_environment_inputs(context, offline=offline)
            _prepare_rootfs(
                kern,
                candidate,
                context=context,
                lock=lock,
                recipe=environment_recipe,
                generation=generation,
                reporter=reporter,
            )
        if environment_recipe_digest(lock) != environment_recipe:
            fail("environment inputs changed while setup was running")
        state = probe_environment_state(kern, candidate, environment_recipe)
        if state is None or state.generation != generation:
            fail("environment preparation did not publish the exact requested generation")
        state = publish_environment(kern, candidate, state)
    except BaseException:
        failed = True
        raise
    finally:
        if candidate.exists():
            try:
                remove_environment_rootfs(candidate)
            except BaseException as error:
                if not failed:
                    raise
                print(
                    error_message(f"could not remove failed environment candidate: {error}"),
                    file=sys.stderr,
                )
    install_git_hooks()
    if own_reporter:
        reporter.finish()
    return state

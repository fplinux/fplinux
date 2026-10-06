# SPDX-License-Identifier: GPL-2.0-only
"""Run quality commands against an immutable checkout in the pinned Kern image."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from fplinux_cli.environment.image_store import current_image_state, publish_current_image_state
from fplinux_cli.environment.images import container_image_reference
from fplinux_cli.environment.kern import (
    kern_available,
    kern_box_name,
    kern_environment,
    require_kern,
)
from fplinux_cli.environment.setup import setup

if TYPE_CHECKING:
    from pathlib import Path

    from fplinux_cli.environment.image_state import ImageState
    from fplinux_cli.reporting.run import RunReporter, Stage

_QUALITY_COMMAND_TIMEOUT = 2 * 60 * 60


def prepare_quality_image(
    reporter: RunReporter, lock: dict[str, Any], image_recipe: str
) -> tuple[str, str, ImageState]:
    """Reuse an inspected image generation or prepare the pinned environment."""
    image = container_image_reference(lock, image_recipe)
    if kern_available(lock):
        kern = require_kern(lock)
        inspected = current_image_state(kern, image, image_recipe)
        if inspected is not None:
            state = publish_current_image_state(kern, image, image_recipe, state=inspected)
        else:
            state = setup(reporter=reporter, lock=lock, image_recipe=image_recipe)
    else:
        state = setup(reporter=reporter, lock=lock, image_recipe=image_recipe)
        kern = require_kern(lock)
    return kern, image, state


def run_quality_command(  # noqa: PLR0913 -- runtime, deadline and log ownership stay explicit.
    stage: Stage,
    workspace: Path,
    command: list[str],
    *,
    kern: str,
    image: str,
    timeout: float = _QUALITY_COMMAND_TIMEOUT,
    log_name: str | None = None,
) -> None:
    """Keep sources read-only, network disabled and full child logs in the run directory."""
    reporter = stage.reporter
    name = log_name or stage.name
    logs = reporter.root / "containers"
    logs.mkdir(parents=True, exist_ok=True)
    environment = reporter.container_environment(f"/logs/{name}")
    environment["FPLINUX_LOG_DISPLAY_ROOT"] += f"/containers/{name}"
    log_arguments = [
        part for key, value in environment.items() for part in ("--env", f"{key}={value}")
    ]
    stage.run(
        [
            kern,
            "box",
            kern_box_name(f"{reporter.label}-{name}"),
            "--image",
            image,
            "--pull",
            "never",
            "--read-only",
            "--network",
            "none",
            # Kern creates private devpts for real PTY component tests with -t.
            "-t",
            "--tmpfs",
            "/tmp:1g",  # noqa: S108 -- container tmpfs.
            "--volume",
            f"{workspace}:/workspace:ro",
            "--volume",
            f"{logs}:/logs",
            *log_arguments,
            "--env",
            "HOME=/tmp",
            "--env",
            "PYTHONPATH=/workspace/scripts",
            "--env",
            "RUFF_CACHE_DIR=/tmp/ruff",
            "--env",
            "PYTHONDONTWRITEBYTECODE=1",
            "--workdir",
            "/workspace",
            "--init",
            "--quiet",
            "--",
            *command,
        ],
        env=kern_environment(),
        timeout=timeout,
    )

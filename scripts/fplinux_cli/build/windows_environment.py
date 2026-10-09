# SPDX-License-Identifier: GPL-2.0-only
"""Run native Windows build steps in the pinned project image."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.environment.image_store import current_image_state
from fplinux_cli.environment.images import (
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from fplinux_cli.environment.kern import kern_box_name, kern_environment, require_kern

if TYPE_CHECKING:
    from pathlib import Path


class WindowsBuildEnvironment:
    """Own one writable build directory and explicit read-only input mounts."""

    def __init__(self, *, work: Path, inputs: list[Path]) -> None:
        """Require the exact project runtime and its current build image."""
        self.work = work.resolve()
        self.inputs = [path.resolve() for path in inputs]
        lock = load_container_lock()
        self.kern = require_kern(lock)
        recipe = container_image_recipe_digest(lock)
        self.image = container_image_reference(lock, recipe)
        if current_image_state(self.kern, self.image, recipe) is None:
            common.fail("Windows packaging requires the current pinned image; run ./fplinux setup")

    def _command(
        self,
        command: list[str],
        *,
        cwd: Path,
        variables: dict[str, str] | None = None,
    ) -> list[str]:
        volumes = [f"{self.work}:{self.work}", f"{common.ROOT / 'scripts'}:/workspace/scripts:ro"]
        volumes.extend(f"{path}:{path}:ro" for path in self.inputs)
        return [
            self.kern,
            "box",
            kern_box_name("windows-build-tools"),
            "--image",
            self.image,
            "--pull",
            "never",
            "--read-only",
            "--network",
            "none",
            "--no-uid-range",
            "--tmpfs",
            "/tmp:128m",  # noqa: S108 -- container tmpfs.
            *[argument for volume in volumes for argument in ("--volume", volume)],
            "--env",
            "HOME=/tmp",
            "--env",
            "PYTHONPATH=/workspace/scripts",
            "--env",
            "PYTHONDONTWRITEBYTECODE=1",
            *[
                argument
                for name, value in (variables or {}).items()
                for argument in ("--env", f"{name}={value}")
            ],
            "--workdir",
            str(cwd),
            "--init",
            "--quiet",
            "--",
            *command,
        ]

    def run(
        self,
        command: list[str],
        *,
        cwd: Path,
        variables: dict[str, str] | None = None,
    ) -> None:
        """Run a native compiler, resource embedder, extractor or patch operation."""
        subprocess.run(
            self._command(command, cwd=cwd, variables=variables),
            env=kern_environment(),
            check=True,
        )

    def inspect(self, path: Path) -> subprocess.CompletedProcess[str]:
        """Inspect a PE file using the image's native binutils without executing it."""
        return subprocess.run(
            self._command(["objdump", "-p", str(path)], cwd=self.work),
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
        )

    def apply_patches(self, source: Path, patches: list[Path]) -> None:
        """Use the existing source projection's ordered, fuzz-free patch operation."""
        if not patches:
            return
        self.run(
            [
                "python3",
                "-c",
                (
                    "import sys; from pathlib import Path; "
                    "from fplinux_cli.build.sources import apply_patches; "
                    "apply_patches(Path(sys.argv[1]), [Path(path) for path in sys.argv[2:]])"
                ),
                str(source),
                *[str(path) for path in patches],
            ],
            cwd=source,
        )

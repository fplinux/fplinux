# SPDX-License-Identifier: GPL-2.0-only
"""Run native Windows build steps in the pinned project environment."""

from __future__ import annotations

import subprocess
from typing import TYPE_CHECKING

from fplinux_cli import common
from fplinux_cli.data_paths import data_root
from fplinux_cli.environment.definition import (
    environment_recipe_digest,
    load_environment_lock,
)
from fplinux_cli.environment.kern import kern_box_name, kern_environment, require_kern
from fplinux_cli.environment.runtime import (
    current_environment_state,
    environment_arguments,
    environment_rootfs,
)

if TYPE_CHECKING:
    from pathlib import Path


class WindowsBuildEnvironment:
    """Own one writable build directory and explicit read-only input mounts."""

    def __init__(self, *, work: Path, inputs: list[Path]) -> None:
        """Require the exact project runtime and its current build environment."""
        self.work = work.resolve()
        self.inputs = [path.resolve() for path in inputs]
        self.cache = data_root(common.ROOT)
        lock = load_environment_lock()
        self.kern = require_kern(lock, cache=self.cache)
        recipe = environment_recipe_digest(lock)
        state = current_environment_state(self.kern, recipe, cache=self.cache)
        if state is None:
            common.fail(
                "Windows packaging requires the current pinned environment; run ./fplinux setup"
            )
        self.rootfs = environment_rootfs(self.cache, state)

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
            *environment_arguments(self.rootfs),
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
            env=kern_environment(cache=self.cache),
            check=True,
        )

    def inspect(self, path: Path) -> subprocess.CompletedProcess[str]:
        """Inspect a PE file using the environment's native binutils without executing it."""
        return subprocess.run(
            self._command(["objdump", "-p", str(path)], cwd=self.work),
            env=kern_environment(cache=self.cache),
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

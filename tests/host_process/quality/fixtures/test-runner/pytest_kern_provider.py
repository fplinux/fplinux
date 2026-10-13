# SPDX-License-Identifier: GPL-2.0-only
"""Translate the controlled Kern launch into a real local pytest process."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def translated(value: str, mounts: dict[str, str]) -> str:
    """Resolve mounted paths in the controlled command and environment."""
    for target, source in mounts.items():
        value = value.replace(target, source)
    return value


def main() -> None:
    """Replace external isolation while retaining arguments and actual pytest execution."""
    arguments = sys.argv[1:]
    boundary = arguments.index("--")
    options = arguments[:boundary]
    mounts = {}
    for index, option in enumerate(options):
        if option == "--volume":
            source, target, *_ = options[index + 1].split(":")
            mounts[target] = source

    environment = os.environ.copy()
    for index, option in enumerate(options):
        if option == "--env":
            name, value = options[index + 1].split("=", 1)
            environment[name] = translated(value, mounts)
    environment.pop("PYTEST_ADDOPTS", None)
    environment.pop("PYTEST_PLUGINS", None)
    environment["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    environment["FPLINUX_TEST_RUNTIME"] = "container"
    environment["PYTHONNOUSERSITE"] = "1"
    environment["PYTHONPATH"] = os.pathsep.join(
        (environment["PYTHONPATH"], os.environ["FPLINUX_TEST_DEPENDENCIES"])
    )
    os.chdir(translated(options[options.index("--workdir") + 1], mounts))
    command = [translated(value, mounts) for value in arguments[boundary + 1 :]]
    with Path(os.environ["FPLINUX_TEST_KERN_TRACE"]).open("a") as stream:
        stream.write("container\n")
    os.execve(sys.executable, [sys.executable, *command[1:]], environment)


if __name__ == "__main__":
    main()

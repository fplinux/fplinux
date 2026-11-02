# SPDX-License-Identifier: GPL-2.0-only
"""Check native smoke discovery when Unix-only modules are unavailable."""

import importlib.abc
import sys


class RejectUnixImports(importlib.abc.MetaPathFinder):
    """Reject only modules absent from the native Windows standard library."""

    def find_spec(self, fullname: str, _path: object = None, _target: object = None) -> None:
        """Let other modules proceed through the ordinary import system."""
        if fullname in {"pwd", "grp", "fcntl", "termios", "resource"}:
            raise ModuleNotFoundError(f"module unavailable on Windows: {fullname}")


for name in ("pwd", "grp", "fcntl", "termios", "resource"):
    sys.modules.pop(name, None)
sys.meta_path.insert(0, RejectUnixImports())

from fplinux_cli.ci import smoke  # noqa: E402 -- controlled import boundary.
from fplinux_cli.packaging.distribution import (  # noqa: E402 -- controlled import boundary.
    distribution_matrix,
)

if not callable(smoke.smoke_distribution) or not distribution_matrix():
    message = "native smoke could not discover its archive selections"
    raise SystemExit(message)
print("native smoke discovery without Unix modules: OK")

# SPDX-License-Identifier: GPL-2.0-only
"""Pack RAM-root files through the staged checkout selected by the caller."""

import sys
from pathlib import Path

# Preserve the original -c search path before loading the staged implementation.
sys.path[0] = ""
from fplinux_cli.alpine.rootfs_files import _write_ramroot_initramfs  # noqa: E402

_write_ramroot_initramfs(Path(sys.argv[1]), Path(sys.argv[2]))

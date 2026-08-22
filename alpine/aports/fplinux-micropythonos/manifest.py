# SPDX-License-Identifier: MIT
# ruff: noqa: INP001, D100, F821
# mypy: disable-error-code=name-defined
include("$(MPY_DIR)/extmod/asyncio")
freeze("../internal_filesystem/", "main.py")
freeze("../internal_filesystem/lib", "")
freeze("../freezeFS/", "freezefs_mount_builtin.py")

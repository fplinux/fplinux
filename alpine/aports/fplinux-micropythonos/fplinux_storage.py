# SPDX-License-Identifier: MIT
# ruff: noqa: A002, ANN001, ANN002, ANN003, ANN201, ANN202, ANN206, D102, FBT002, INP001, PLC0415, PTH123, PTH208, UP015
# mypy: ignore-errors
"""Mounted Linux storage capability for MicroPythonOS."""

import os


def _storage_path():
    path = os.getenv("MPOS_STORAGE", "")
    if not path or not path.startswith("/"):
        return None
    if path != "/" and path.endswith("/"):
        return None
    return path


def _is_exact_mount(path):
    try:
        with open("/proc/self/mountinfo", "r") as mountinfo:
            for line in mountinfo:
                fields = line.split()
                if len(fields) > 5 and fields[4] == path:
                    return True
    except OSError:
        pass
    return False


class FPLinuxStorage:
    """Expose one target-declared removable filesystem mounted by the wrapper."""

    @classmethod
    def init(cls, *args, **kwargs):
        del args, kwargs

    @classmethod
    def mount(cls, format=False):
        if format:
            return False
        return cls.is_mounted()

    @classmethod
    def is_mounted(cls):
        path = _storage_path()
        return path is not None and _is_exact_mount(path)

    @classmethod
    def get_mount_point(cls):
        if cls.is_mounted():
            return _storage_path()
        return None

    @classmethod
    def get_mode(cls):
        return "linux" if cls.is_mounted() else None

    @classmethod
    def format(cls):
        return False

    @classmethod
    def get_raw(cls):
        return None

    @classmethod
    def list(cls, mount_point=None):
        path = cls.get_mount_point()
        if path is None:
            return []
        if mount_point is not None and mount_point.rstrip("/") != path:
            return []
        try:
            return os.listdir(path)
        except OSError:
            return []


def install():
    """Replace MCU-only SDCardManager bindings with the Linux capability."""
    import mpos
    import mpos.sdcard

    mpos.SDCardManager = FPLinuxStorage
    mpos.sdcard.SDCardManager = FPLinuxStorage
    try:
        from mpos.ui import file_explorer_activity

        file_explorer_activity.SDCardManager = FPLinuxStorage
    except ImportError:
        pass

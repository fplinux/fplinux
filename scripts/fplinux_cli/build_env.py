# SPDX-License-Identifier: GPL-2.0-only
"""Shared deterministic environment for target, bootstrap and Alpine builds."""

from __future__ import annotations

import os

SOURCE_DATE_EPOCH = "1784919600"


def build_environment() -> dict[str, str]:
    """Return the deterministic environment shared by every build layer."""
    return {
        **os.environ,
        "LC_ALL": "C",
        "SOURCE_DATE_EPOCH": SOURCE_DATE_EPOCH,
        "KBUILD_BUILD_TIMESTAMP": "2026-07-24 19:00:00 +0000",
        "KBUILD_BUILD_USER": "fplinux",
        "KBUILD_BUILD_HOST": "builder",
        "KBUILD_BUILD_VERSION": "1",
        "KCONFIG_NOTIMESTAMP": "1",
    }

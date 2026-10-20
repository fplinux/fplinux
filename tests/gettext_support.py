# SPDX-License-Identifier: GPL-2.0-only
"""Link flags for host components whose sources call gettext()."""

from pathlib import Path


def gettext_link_flags() -> list[str]:
    """Link libintl when the installed header redirects gettext to that library."""
    header = Path("/usr/include/libintl.h")
    if header.is_file() and "libintl_gettext" in header.read_text(errors="replace"):
        return ["-lintl"]
    return []

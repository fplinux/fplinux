# SPDX-License-Identifier: GPL-2.0-only
"""Name the source archive in the runnable ZIP."""

from textwrap import dedent


def append_source_companion(readme: bytes, source_stem: str) -> bytes:
    """Append the matching source archive after its name has been determined."""
    footer = dedent("""\
        Matching source and build material:
          {source_stem}.zip
        Keep it available alongside this archive. Licenses remain included here.
        """).format(source_stem=source_stem)
    return readme + b"\n" + footer.encode("utf-8")

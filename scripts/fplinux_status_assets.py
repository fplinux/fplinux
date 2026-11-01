#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build early-display assets from the verified UI font and profile inputs."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

import fplinux_ui_fonts
import fplinux_ui_profile


def main(argv: list[str] | None = None) -> int:
    """Convert with the UI producer, then export the chosen early-screen roles."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tokens", type=Path, required=True)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=fplinux_ui_profile.PROFILES)
    parser.add_argument("--lv-font-conv", default="lv_font_conv")
    args = parser.parse_args(argv)
    try:
        tokens = json.loads(args.tokens.read_text(encoding="utf-8"))
        fonts = fplinux_ui_fonts.plan(tokens)
        output = args.output
        output.mkdir(parents=True, exist_ok=True)
        converted = output / "ui-fonts"
        fplinux_ui_fonts.build(fonts, args.sources, converted, args.lv_font_conv)
        (output / "fplinux-status-fonts.c").write_text(
            fplinux_ui_fonts.status_source(tokens, converted, args.profile), encoding="utf-8"
        )
        if args.profile:
            (output / "fplinux-status-profile.h").write_text(
                fplinux_ui_profile.render_status(tokens, args.profile), encoding="utf-8"
            )
        else:
            import fplinux_activity  # noqa: PLC0415 -- Linux activity assets only.

            (output / "fplinux-activity-masks.h").write_text(
                fplinux_activity.render(tokens), encoding="utf-8"
            )
        # The source manifest contains the verified faces and their OFL notices.
        _, originals = fplinux_ui_fonts.source_manifest(args.sources / "fplinux-font-sources.json")
        notices = output / "licenses"
        notices.mkdir(exist_ok=True)
        for name in originals:
            if name.endswith(".txt"):
                shutil.copyfile(args.sources / name, notices / name)
    except (
        OSError,
        ValueError,
        subprocess.SubprocessError,
        fplinux_ui_fonts.FontBuildError,
    ) as error:
        print(f"status assets: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

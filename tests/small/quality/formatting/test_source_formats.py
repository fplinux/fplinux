# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for the shared source formatter classification."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.quality.formatting.source_formats import classify_source_formats

if TYPE_CHECKING:
    from pathlib import Path


class SourceFormatClassificationTests:
    """Keep check and format on one observable file-to-tool contract."""

    def test_current_formatter_boundaries_are_classified_once(self, tmp_path: Path) -> None:
        """Route canonical source types and preserve external-owner exclusions."""
        root = tmp_path
        contents = {
            "tool.py": "value=1\n",
            "README.md": "# Demo\n",
            "data.json": "{}\n",
            "settings.jsonc": "{}\n",
            "package-lock.json": "{}\n",
            "target.toml": "name='demo'\n",
            "driver.c": "int value;\n",
            "driver.h": "int value;\n",
            "script.sh": "#!/bin/sh\necho ok\n",
            "service.initd": "#!/sbin/openrc-run\n",
            "helper": "#!/usr/bin/env bash\necho ok\n",
            "terminal.bashrc": "#!/usr/bin/env bash\nbind 'set bell-style none'\n",
            "binding.yaml": "---\n",
            "README.md.in": "# @DEVICE@\n",
            "target.toml.in": "name='@DEVICE@'\n",
            "phone.dts.in": '/dts-v1/;\n/ { model = "@DEVICE@"; };\n',
            "Kconfig.in": 'config DEMO\n\tbool "@DEVICE@"\n',
            "Makefile.in": "NAME = @TARGET@\n",
            ".editorconfig": "root = true\n",
            "notes.txt": "plain\n",
            "other.conf": "setting=value\n",
            "other.mjs": "export default {};\n",
            "other.JSON": "{}\n",
        }
        for relative, text in contents.items():
            (root / relative).write_text(text, encoding="utf-8")

        formats = classify_source_formats(
            [root / relative for relative in contents],
            root=root,
        )

        assert (formats.python) == (("tool.py",))
        assert (formats.markdown) == (("README.md", "README.md.in"))
        assert (formats.json) == (("data.json", "settings.jsonc"))
        assert (formats.toml) == (("target.toml", "target.toml.in"))
        assert (formats.posix_shell) == (("script.sh", "service.initd"))
        assert (formats.bash) == (("helper", "terminal.bashrc"))
        assert (formats.c) == (("driver.c", "driver.h"))
        assert ("package-lock.json") not in (formats.supported())
        assert (formats.yaml) == (("binding.yaml",))
        assert (formats.devicetree) == (("phone.dts.in",))
        assert (formats.text) == ((".editorconfig", "Kconfig.in", "Makefile.in"))
        assert ("notes.txt") not in (formats.supported())
        assert ("other.conf") not in (formats.supported())
        assert ("other.mjs") not in (formats.supported())
        assert ("other.JSON") not in (formats.supported())

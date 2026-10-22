# SPDX-License-Identifier: GPL-2.0-only
"""Generate the installed alpha masks used by application actions."""

from __future__ import annotations

import importlib
import json
import os
import shutil
import sys
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    from types import ModuleType


@pytest.fixture(scope="module")
def generated_glyphs(tmp_path_factory: pytest.TempPathFactory) -> Path:
    """Replace external SVG downloads, while running the installed-file generator."""
    directory = tmp_path_factory.mktemp("system-glyphs")
    aport = ROOT / "alpine/aports/fplinux-icons"
    manifest = json.loads((aport / "icons.json").read_text(encoding="utf-8"))
    glyph = Path(__file__).with_name("fixtures") / "icons/glyph.svg"
    tabler = directory / "tabler"
    for entry in manifest["glyphs"].values():
        if entry["source"] != "tabler":
            continue
        for style in set(entry["styles"].values()):
            source = tabler / style / f"{entry['name']}.svg"
            source.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(glyph, source)
    for entry in manifest["apps"].values():
        source = tabler / "outline" / f"{entry['glyph']}.svg"
        source.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(glyph, source)
    output = directory / "output"
    run_process(
        [
            sys.executable,
            str(aport / "generate-icons.py"),
            "--tabler",
            str(tabler),
            "--profile",
            str(ROOT / "lib/fplinux/fplinux-ui-profile.json"),
            "--out",
            str(output),
        ],
        name="generate system action glyph resources",
        env={**os.environ, "NODE_PATH": "/opt/quality/node-tools/node_modules"},
        timeout=30,
        check=True,
    )
    return output / "usr/share/fplinux/glyphs"


@pytest.fixture
def png_reader(monkeypatch: pytest.MonkeyPatch) -> ModuleType:
    """Read generated PNG pixels through the shared raster module's normal import."""
    monkeypatch.syspath_prepend(str(ROOT / "alpine/aports/fplinux-icons"))
    return importlib.import_module("svgraster")


@pytest.mark.parametrize("size", [12, 16, 20, 24])
@pytest.mark.parametrize(
    "action",
    [
        "save",
        "send",
        "edit",
        "search",
        "add",
        "delete",
        "help",
        "refresh",
        "backspace",
        "equal",
        "exit",
        "cancel",
        "back",
        "alarm",
        "timer",
        "stopwatch",
        "world",
        "snooze",
        "play",
        "pause",
        "stop",
        "flag",
        "list",
        "settings",
        "check",
        "enter",
        "symbols",
        "history",
        "text-size",
        "keyboard",
        "date-time",
        "no-sim",
    ],
)
def test_application_action_has_an_installed_nonempty_alpha_mask_at_each_size(
    generated_glyphs: Path, png_reader: ModuleType, action: str, size: int
) -> None:
    image = png_reader.decode_png((generated_glyphs / str(size) / f"{action}.png").read_bytes())
    assert (image.width, image.height) == (size, size)
    assert max(image.alpha()) == 255
    assert min(image.alpha()) == 0
    assert all(image.pixels[channel::4] == bytes(size * size) for channel in (0, 1, 2))

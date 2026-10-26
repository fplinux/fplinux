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
def generated_icons(tmp_path_factory: pytest.TempPathFactory) -> Path:
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
    return directory


@pytest.fixture(scope="module")
def generated_glyphs(generated_icons: Path) -> Path:
    return generated_icons / "output/usr/share/fplinux/glyphs"


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


def test_application_tiles_use_profile_sizes_and_radii_without_a_second_size_list(
    generated_icons: Path, png_reader: ModuleType, tmp_path: Path
) -> None:
    """The installed PNGs follow a changed profile with no duplicate declaration."""
    aport = ROOT / "alpine/aports/fplinux-icons"
    sources = generated_icons
    manifest = json.loads((aport / "icons.json").read_text(encoding="utf-8"))
    manifest.pop("tile_sizes", None)
    profile = json.loads(
        (ROOT / "lib/fplinux/fplinux-ui-profile.json").read_text(encoding="utf-8")
    )
    profile["tile_radius"]["16"] = 7
    profile["tile_radius"]["64"] = 14
    manifest["tile_glyph"]["64"] = {"glyph": 40, "stroke": 2}
    manifest_path = tmp_path / "icons.json"
    profile_path = tmp_path / "profile.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    profile_path.write_text(json.dumps(profile), encoding="utf-8")
    output = tmp_path / "output"
    result = run_process(
        [
            sys.executable,
            str(aport / "generate-icons.py"),
            "--tabler",
            str(sources / "tabler"),
            "--manifest",
            str(manifest_path),
            "--profile",
            str(profile_path),
            "--out",
            str(output),
        ],
        name="generate application tiles from the profile's size and radius table",
        env={**os.environ, "NODE_PATH": "/opt/quality/node-tools/node_modules"},
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    sizes = (16, 20, 24, 32, 40, 48, 56, 60, 64)
    installed = output / "usr/share/icons/hicolor"
    assert {entry.name for entry in installed.iterdir()} == {f"{size}x{size}" for size in sizes}
    apps = {
        "org.fplinux.clock",
        "org.fplinux.calculator",
        "org.fplinux.settings",
        "org.fplinux.snake",
        "org.fplinux.notes",
        "org.fplinux.terminal",
    }
    for size in sizes:
        directory = installed / f"{size}x{size}/apps"
        assert {entry.stem for entry in directory.iterdir()} == apps
        for path in directory.iterdir():
            image = png_reader.decode_png(path.read_bytes())
            assert (image.width, image.height) == (size, size)
            if size not in (16, 64):
                original = sources / "output/usr/share/icons/hicolor" / path.relative_to(installed)
                assert path.read_bytes() == original.read_bytes()
    changed = png_reader.decode_png((installed / "16x16/apps/org.fplinux.snake.png").read_bytes())
    original = png_reader.decode_png(
        (sources / "output/usr/share/icons/hicolor/16x16/apps/org.fplinux.snake.png").read_bytes()
    )
    assert changed.alpha()[2] == 0
    assert original.alpha()[2] > 0
    assert changed.alpha()[8] == 255
    assert changed.alpha() != original.alpha()

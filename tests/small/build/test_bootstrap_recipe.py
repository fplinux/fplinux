# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for the device-visible bootstrap recipe closure."""

from __future__ import annotations

import io
import json
import tarfile
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build.bootstrap import recipe as bootstrap_build
from fplinux_cli.build.bootstrap import stage as bootstrap_stage
from fplinux_cli.manifests.targets import load_target

from tests import ROOT

BOOTSTRAP_FILES = ROOT / "tests/fixtures/bootstrap-recipe"

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path


class BootstrapContext:
    """Hash only inputs copied or selected by ``build_bootstrap``."""

    def __init__(self, root: Path) -> None:
        """Create a minimal target/bootstrap projection closure."""
        self.root = root
        self.write("targets/demo/bootstrap/Makefile", b"all:\n\ttrue\n")
        self.write("targets/demo/bootstrap/main.c", b"int entry(void) { return 1; }\n")
        self.write("bootstrap/fplinux-boot-screen/screen.c", b"int screen;\n")
        self.write("bootstrap/fplinux-boot-screen/font.h", b"font declarations\n")
        self.write("patches/vendor.patch", b"vendor patch\n")
        for relative, _source in inputs_build.bootstrap_implementation_sources():
            self.write(relative, b"builder implementation\n")
        for owner in ("environment/downloads.py", "manifests/values.py"):
            relative = f"scripts/fplinux_cli/{owner}"
            self.write(relative, (ROOT / relative).read_bytes())
        self.write("scripts/fplinux_cli/build/environment.py", b"build environment\n")
        self.target_config = json.loads(
            (BOOTSTRAP_FILES / "target.json").read_text(encoding="utf-8")
        )
        self.platform = json.loads((BOOTSTRAP_FILES / "platform.json").read_text(encoding="utf-8"))
        self.sources = common.load_toml(BOOTSTRAP_FILES / "sources.lock.toml")

    def write(self, relative: str, contents: bytes) -> Path:
        """Write one test repository file."""
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def digest(self) -> str:
        """Compute the bootstrap recipe against the temporary repository."""
        with mock.patch.object(common, "ROOT", self.root):
            return bootstrap_build.bootstrap_recipe_digest(
                self.sources,
                "demo",
                self.target_config,
                self.platform,
            )


@pytest.fixture
def bootstrap_context(tmp_path: Path) -> BootstrapContext:
    """Supply one owned target and bootstrap source projection."""
    return BootstrapContext(tmp_path)


class BootstrapRecipeTests:
    """Hash only inputs copied or selected by ``build_bootstrap``."""

    @pytest.mark.parametrize(
        "changed_input",
        [
            "target-source",
            "shared-copy",
            "vendor-patch",
            "vendor-archive",
            "vendor-commit",
            "toolchain",
            "build-targets",
            "build-environment",
            "verified-transfer",
            "sha-validation",
            "display-name",
            "record-prefix",
            "geometry",
            "boot-font",
            "image-name",
            "map-name",
            "dtb-name",
        ],
    )
    def test_bootstrap_source_shared_vendor_and_config_are_causal(
        self, bootstrap_context: BootstrapContext, changed_input: str
    ) -> None:
        """Every declared bootstrap input changes the exact recipe."""
        changes: dict[str, Callable[[], object]] = {
            "target-source": lambda: bootstrap_context.write(
                "targets/demo/bootstrap/main.c", b"int entry(void) { return 2; }\n"
            ),
            "shared-copy": lambda: bootstrap_context.write(
                "bootstrap/fplinux-boot-screen/screen.c", b"int changed_screen;\n"
            ),
            "vendor-patch": lambda: bootstrap_context.write(
                "patches/vendor.patch", b"changed vendor patch\n"
            ),
            "vendor-archive": lambda: bootstrap_context.sources["vendor"].update(
                archive_sha256="b" * 64
            ),
            "vendor-commit": lambda: bootstrap_context.sources["vendor"].update(commit="def456"),
            "toolchain": lambda: bootstrap_context.platform["bootstrap"].update(
                toolchain="arm-none-eabi-custom"
            ),
            "build-targets": lambda: bootstrap_context.platform["bootstrap"].update(
                build_targets=["clean", "all"]
            ),
            "build-environment": lambda: bootstrap_context.write(
                "scripts/fplinux_cli/build/environment.py", b"changed environment\n"
            ),
            "verified-transfer": lambda: bootstrap_context.write(
                "scripts/fplinux_cli/environment/downloads.py", b"changed verified transfer\n"
            ),
            "sha-validation": lambda: bootstrap_context.write(
                "scripts/fplinux_cli/manifests/values.py", b"changed value validation\n"
            ),
            "display-name": lambda: bootstrap_context.target_config["identity"].update(
                display_name="Demo Changed Phone"
            ),
            "record-prefix": lambda: bootstrap_context.target_config["bootstrap"].update(
                record_prefix="CHANGED"
            ),
            "geometry": lambda: bootstrap_context.target_config["display"].update(
                width=128, height=160
            ),
            "boot-font": lambda: bootstrap_context.target_config["display"].update(
                boot_font="7x14"
            ),
            "image-name": lambda: bootstrap_context.target_config["bootstrap"].update(
                image="changed.bin"
            ),
            "map-name": lambda: bootstrap_context.target_config["bootstrap"].update(
                map="obj/changed.map"
            ),
            "dtb-name": lambda: bootstrap_context.target_config["bootstrap"].update(
                dtb_destination="changed.dtb"
            ),
        }
        baseline = bootstrap_context.digest()
        changes[changed_input]()
        assert (baseline) != (bootstrap_context.digest())

    def test_host_and_docs_are_outside_the_bootstrap_closure(
        self, bootstrap_context: BootstrapContext
    ) -> None:
        """Unselected host and documentation files must not relabel the phone image."""
        baseline = bootstrap_context.digest()
        documentation = bootstrap_context.write("docs/BUILDING.md", b"unrelated documentation\n")
        assert (baseline) == (bootstrap_context.digest())
        documentation.unlink()

        host_adapter = bootstrap_context.write(
            "platforms/demo/host/adapter.py",
            b"unrelated host adapter\n",
        )
        assert (baseline) == (bootstrap_context.digest())
        host_adapter.unlink()

        bootstrap_context.sources["vendor"]["repository"] = "https://example.invalid/other"
        assert (baseline) == (bootstrap_context.digest())
        bootstrap_context.sources["vendor"]["repository"] = "https://example.invalid/vendor"

        bootstrap_context.sources["vendor"]["archive_url"] = "https://example.invalid/other.tar.gz"
        assert (baseline) == (bootstrap_context.digest())
        bootstrap_context.sources["vendor"]["archive_url"] = (
            "https://example.invalid/vendor.tar.gz"
        )

        bootstrap_context.sources["vendor"]["license"] = "Other"
        assert (baseline) == (bootstrap_context.digest())

    def test_sd_recipe_tracks_the_shared_target_panel(
        self, bootstrap_context: BootstrapContext
    ) -> None:
        """The RAM-owned panel also invalidates its SD consumer without foreign panel inputs."""
        bootstrap_context.write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        panel = bootstrap_context.write("targets/demo/bootstrap/lcd_config.h", b"selected panel\n")
        bootstrap_context.target_config["bootstrap"].update(
            source="bootstrap-microsd", lcd_config="bootstrap/lcd_config.h"
        )
        baseline = bootstrap_context.digest()

        bootstrap_context.write("targets/another/bootstrap/lcd_config.h", b"foreign panel\n")
        assert (baseline) == (bootstrap_context.digest())
        panel.write_bytes(b"changed selected panel\n")
        assert (baseline) != (bootstrap_context.digest())

    @pytest.mark.parametrize("source", ["bootstrap", "bootstrap-microsd"])
    def test_rendered_panel_data_changes_either_bootstrap_recipe(
        self, bootstrap_context: BootstrapContext, source: str
    ) -> None:
        """A selected LCD command change invalidates either stage without foreign inputs."""
        bootstrap_context.write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        bootstrap_context.write("targets/demo/bootstrap/lcd_config.h", b"selected panel wrapper\n")
        bootstrap_context.target_config["display"] = load_target("nokia-ta1618")["display"]
        commands: list[dict[str, Any]] = [{"command": 0x36, "data": [0x28], "delay_ms": 0}]
        bootstrap_context.target_config["display"]["panel"]["commands"] = commands
        bootstrap_context.target_config["bootstrap"].update(
            source=source, lcd_config="bootstrap/lcd_config.h"
        )
        baseline = bootstrap_context.digest()
        bootstrap_context.write("targets/another/display/panel.toml", b"unselected panel\n")
        assert baseline == bootstrap_context.digest()

        commands[0]["data"][0] = 0x48
        assert baseline != bootstrap_context.digest()

    @pytest.mark.parametrize("source", ["bootstrap", "bootstrap-microsd"])
    def test_linux_font_source_and_freestanding_header_are_causal(
        self, bootstrap_context: BootstrapContext, source: str
    ) -> None:
        """A pinned font or its compile interface invalidates either bootstrap recipe."""
        bootstrap_context.write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        bootstrap_context.target_config["bootstrap"]["source"] = source
        baseline = bootstrap_context.digest()
        bootstrap_context.sources["linux"]["url"] = "https://example.invalid/mirror.tar.xz"
        bootstrap_context.write("unrelated-font.c", b"int unrelated_font;\n")
        assert (baseline) == (bootstrap_context.digest())
        bootstrap_context.sources["linux"]["sha256"] = "d" * 64
        assert (baseline) != (bootstrap_context.digest())
        bootstrap_context.sources["linux"]["sha256"] = "c" * 64
        header = bootstrap_context.write(
            "bootstrap/fplinux-boot-screen/font.h", b"changed declarations\n"
        )
        assert (baseline) != (bootstrap_context.digest())
        header.write_bytes(b"font declarations\n")

    @pytest.mark.parametrize(
        ("source", "selected", "expected"),
        [
            pytest.param(
                "bootstrap", "bootstrap/lcd_config.h", b"single selected panel\n", id="ram-panel"
            ),
            pytest.param(
                "bootstrap-microsd",
                "bootstrap/lcd_config.h",
                b"single selected panel\n",
                id="microsd-panel",
            ),
            pytest.param("bootstrap", None, b"complete porting table\n", id="headless-table"),
        ],
    )
    def test_projection_selects_one_panel_or_the_headless_table(
        self,
        bootstrap_context: BootstrapContext,
        source: str,
        selected: str | None,
        expected: bytes,
    ) -> None:
        """The compiler receives the target bytes for either source tree, or the full fallback."""
        archive = bootstrap_context.root / "cache/downloads/vendor.tar.gz"
        archive.parent.mkdir(parents=True)
        with tarfile.open(archive, "w:gz") as output:
            for path, contents in (
                ("pack_reloc/Makefile", b"all:\n\ttrue\n"),
                ("runtime/lcd_config.h", b"complete porting table\n"),
            ):
                member = tarfile.TarInfo(f"vendor-abc123/{path}")
                member.size = len(contents)
                output.addfile(member, io.BytesIO(contents))
        bootstrap_context.sources["vendor"]["archive_sha256"] = common.sha256_file(archive)
        linux_archive = bootstrap_context.root / "cache/downloads/linux/linux-1.2.3.tar.xz"
        linux_archive.parent.mkdir(parents=True)
        with tarfile.open(linux_archive, "w:xz") as output:
            contents = b"int unchanged_upstream_font;\n"
            member = tarfile.TarInfo("linux-1.2.3/lib/fonts/font_sample.c")
            member.size = len(contents)
            output.addfile(member, io.BytesIO(contents))
        bootstrap_context.sources["linux"]["sha256"] = common.sha256_file(linux_archive)
        bootstrap_context.platform["bootstrap"]["patches"] = []
        bootstrap_context.write("targets/demo/bootstrap/lcd_config.h", b"single selected panel\n")
        bootstrap_context.write("targets/demo/bootstrap-microsd/Makefile", b"all:\n\ttrue\n")
        zimage = bootstrap_context.write("kernel/zImage", b"kernel payload\n")
        dtb = bootstrap_context.write("kernel/target.dtb", b"device tree\n")
        work = bootstrap_context.root / "work"
        bootstrap_context.target_config["bootstrap"].update(source=source, lcd_config=selected)
        with (
            mock.patch.object(common, "ROOT", bootstrap_context.root),
            mock.patch.object(inputs_build, "CACHE", bootstrap_context.root / "cache"),
            # Compilation is external to this source-projection check.
            mock.patch.object(
                process_build,
                "run",
                side_effect=RuntimeError("stop before compilation"),
            ),
            pytest.raises(RuntimeError, match="stop before compilation"),
        ):
            bootstrap_stage.build_bootstrap(
                bootstrap_context.sources,
                "demo",
                bootstrap_context.target_config,
                bootstrap_context.platform,
                work=work,
                zimage=zimage,
                dtb=dtb,
            )
        panel = work / "bootstrap/vendor/runtime/lcd_config.h"
        assert (panel.read_bytes()) == (expected)
        font = work / "bootstrap/bootstrap/fplinux-boot-screen/font_sample.c"
        assert (font.read_bytes()) == (b"int unchanged_upstream_font;\n")

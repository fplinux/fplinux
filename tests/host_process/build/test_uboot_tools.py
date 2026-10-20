# SPDX-License-Identifier: GPL-2.0-only
"""Cache-lifecycle tests with a synthetic make-compatible U-Boot source."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build import sources as sources_build
from fplinux_cli.build.storage import stage as storage_build
from fplinux_cli.build.storage import uboot as uboot_tools

if TYPE_CHECKING:
    from collections.abc import Iterator

from tests import ROOT

UBOOT_FIXTURES = ROOT / "tests/fixtures/uboot-tools"


def archive_synthetic_source(root: Path, source: Path) -> Path:
    """Create the compressed source input with the standard tar implementation."""
    archive = root / "source.tar.bz2"
    with tarfile.open(archive, "w:bz2") as output:
        output.add(source, arcname="u-boot-2026.07")
    return archive


def full_uboot_config(archive: Path) -> dict[str, Any]:
    """Bind the synthetic archive and a demo platform's required lines to the build."""
    return {
        "kind": "full",
        "source": "u-boot.lock.toml",
        "archive_prefix": "u-boot-2026.07",
        "required_config": ["CONFIG_TARGET_DEMO_BOARD=y", 'CONFIG_BOOTCOMMAND="demoboot"'],
        "lock": {
            "version": "2026.07",
            "repository": "https://source.denx.de/u-boot/u-boot.git",
            "tag": "v2026.07",
            "commit": "e" * 40,
            "archive_url": "https://example.invalid/u-boot.tar.bz2",
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "license": "GPL-2.0-only",
        },
    }


def full_uboot_layout() -> dict[str, int]:
    """Provide the memory layout independently expected by the producer checks."""
    return {
        "uboot_load": 0x81000000,
        "uboot_size": 0x00100000,
        "uboot_stack": 0x80F00000,
        "fit_load": 0x83200000,
        "fdt_pad": 0x00003000,
    }


def prepare_synthetic_source(source: Path) -> None:
    """Install the explicit fixture programs that model the external source tree."""
    (source / "configs").mkdir(parents=True)
    scripts = source / "scripts"
    scripts.mkdir()
    shutil.copyfile(UBOOT_FIXTURES / "Makefile", source / "Makefile")
    for name in ("fake-build.py", "tool-version.py"):
        shutil.copyfile(UBOOT_FIXTURES / name, scripts / name)


@dataclass(frozen=True)
class UbootInputs:
    """Synthetic source archive, selected configuration and one producer workspace."""

    root: Path
    required_config: str
    defconfig: Path
    build_log: Path
    projection: Path
    archive: Path
    config: dict[str, Any]
    work: Path
    layout: dict[str, int]

    def build(self, *, jobs: int = 1) -> uboot_tools.UbootBuild:
        """Build or reuse the producer output for these exact synthetic inputs."""
        return uboot_tools.build_full(
            self.archive,
            self.config,
            defconfig=self.defconfig,
            projections=[(self.projection, "scripts/build-log.sh")],
            patches=[],
            work=self.work,
            jobs=jobs,
            environment_recipe="a" * 64,
            cross_compile="arm-linux-gnueabi-",
            layout=self.layout,
            required_config=self.config["required_config"],
        )

    def build_log_lines(self) -> list[str]:
        """Read build actions recorded by this case's projected source program."""
        if not self.build_log.exists():
            return []
        return self.build_log.read_text(encoding="utf-8").splitlines()


@pytest.fixture
def uboot_inputs() -> Iterator[UbootInputs]:
    """Own the synthetic external source tree and its projected build log."""
    with ExitStack() as cleanup:
        temporary = tempfile.TemporaryDirectory()
        cleanup.enter_context(temporary)
        root = Path(temporary.name)
        required_config = (UBOOT_FIXTURES / "required.config").read_text(encoding="utf-8")
        source = root / "source/u-boot-2026.07"
        prepare_synthetic_source(source)
        defconfig = root / "ta1618_defconfig"
        defconfig.write_text(required_config, encoding="utf-8")
        build_log = root / "build.log"
        projection = root / "build-log.sh"
        shutil.copyfile(UBOOT_FIXTURES / "build-log.sh", projection)
        cleanup.enter_context(
            mock.patch.dict(os.environ, {"FPLINUX_TEST_BUILD_LOG": str(build_log)})
        )
        archive = archive_synthetic_source(root, source)
        config = full_uboot_config(archive)
        work = root / "work"
        layout = full_uboot_layout()
        yield UbootInputs(
            root, required_config, defconfig, build_log, projection, archive, config, work, layout
        )


class UbootToolsTests:
    """Exercise the producer without claiming a real U-Boot build."""

    def test_full_output_is_reused_across_jobs_and_unrelated_siblings(
        self, uboot_inputs: UbootInputs
    ) -> None:
        """A complete verified output serves the same recipe without another build."""
        first = uboot_inputs.build()
        first_binary = first.binary.read_bytes()

        assert (first.elf.read_bytes()[:4]) == (b"\x7fELF")
        assert (first.config.read_text(encoding="utf-8")) == (uboot_inputs.required_config)
        assert (uboot_inputs.build_log_lines()) == (["build"])

        (uboot_inputs.root / "unrelated-sibling.txt").write_text(
            "unchanged input\n", encoding="utf-8"
        )
        reused = uboot_inputs.build(jobs=8)

        assert (reused.receipt) == (first.receipt)
        assert (reused.binary.read_bytes()) == (first_binary)
        assert (uboot_inputs.build_log_lines()) == (["build"])

    def test_defconfig_miss_and_invalid_cached_outputs_rebuild(
        self, uboot_inputs: UbootInputs
    ) -> None:
        """One declared input changes the result, while missing or altered outputs revoke reuse."""
        first = uboot_inputs.build()
        uboot_inputs.defconfig.write_text(
            uboot_inputs.required_config.replace("CONFIG_TEST_INPUT=one", "CONFIG_TEST_INPUT=two"),
            encoding="utf-8",
        )

        changed = uboot_inputs.build()
        assert (changed.receipt) != (first.receipt)
        assert ("CONFIG_TEST_INPUT=two\n") in (changed.config.read_text(encoding="utf-8"))
        assert (uboot_inputs.build_log_lines()) == (["build", "build"])

        changed.binary.write_bytes(b"tampered")
        rebuilt = uboot_inputs.build()
        assert (rebuilt.receipt) == (changed.receipt)
        assert (rebuilt.binary.read_bytes()) == (b"binary")
        assert (uboot_inputs.build_log_lines()) == (["build", "build", "build"])

        rebuilt.map.unlink()
        restored = uboot_inputs.build()
        assert (restored.receipt) == (changed.receipt)
        assert restored.map.is_file()
        assert (uboot_inputs.build_log_lines()) == (["build", "build", "build", "build"])

    def test_failed_changed_recipe_preserves_previous_complete_output_and_receipt(
        self, uboot_inputs: UbootInputs
    ) -> None:
        """A failed replacement leaves every last-good output and its receipt intact."""
        current = uboot_inputs.build()
        visible_outputs = {
            "elf": current.elf,
            "binary": current.binary,
            "dtb": current.dtb,
            "map": current.map,
            "config": current.config,
            "mkimage": current.mkimage,
            "dumpimage": current.dumpimage,
            "receipt": uboot_inputs.work / "uboot" / uboot_tools.RECEIPT_NAME,
        }
        before = {name: path.read_bytes() for name, path in visible_outputs.items()}
        uboot_inputs.defconfig.write_text(
            uboot_inputs.required_config.replace("CONFIG_TEST_INPUT=one", "CONFIG_TEST_FAIL=y"),
            encoding="utf-8",
        )

        with pytest.raises(subprocess.CalledProcessError):
            uboot_inputs.build()

        after = {name: path.read_bytes() for name, path in visible_outputs.items()}
        assert (after) == (before)
        assert (uboot_inputs.build_log_lines()) == (["build"])

    @pytest.mark.parametrize(
        ("previous", "unsafe"),
        [
            ("# CONFIG_MMC_WRITE is not set", "CONFIG_MMC_WRITE=y"),
            ("CONFIG_ENV_IS_NOWHERE=y", "CONFIG_ENV_IS_IN_FAT=y"),
            ("CONFIG_FIT_FULL_CHECK=y", "# CONFIG_FIT_FULL_CHECK is not set"),
            ("CONFIG_TARGET_DEMO_BOARD=y", "CONFIG_TARGET_OTHER_BOARD=y"),
            ('CONFIG_BOOTCOMMAND="demoboot"', 'CONFIG_BOOTCOMMAND="otherboot"'),
        ],
        ids=[
            "mmc-write",
            "persistent-environment",
            "unchecked-fit",
            "wrong-board",
            "wrong-boot-command",
        ],
    )
    def test_writable_or_unverified_config_cannot_replace_complete_output(
        self, uboot_inputs: UbootInputs, previous: str, unsafe: str
    ) -> None:
        """The producer rejects unsafe compiled configs before publishing them."""
        current = uboot_inputs.build()
        before = current.config.read_bytes()
        uboot_inputs.defconfig.write_text(uboot_inputs.required_config.replace(previous, unsafe))
        with pytest.raises(uboot_tools.UbootToolsError, match="read-only MMC contract"):
            uboot_inputs.build()
        assert (current.config.read_bytes()) == (before)

    def test_profile_preparation_emits_the_selected_target_identity(
        self, uboot_inputs: UbootInputs
    ) -> None:
        """A C preprocessor in the synthetic build consumes the chosen target name."""
        layout_prefixes = (
            "CONFIG_TEXT_BASE=",
            "CONFIG_CUSTOM_SYS_INIT_SP_ADDR=",
            "CONFIG_SYS_LOAD_ADDR=",
            "CONFIG_SYS_FDT_PAD=",
        )
        base = "\n".join(
            line
            for line in uboot_inputs.required_config.splitlines()
            if not line.startswith(layout_prefixes)
        )
        receipts = []
        platform = uboot_inputs.root / "platforms/ums9117"
        (platform / "linux/overlay").mkdir(parents=True)
        shutil.copyfile(ROOT / "platforms/ums9117/platform.toml", platform / "platform.toml")
        for target, defconfig in (
            ("nokia-ta1618", "ta1618_defconfig"),
            ("inoi-240-modern-4g", "inoi240_defconfig"),
            ("inoi-244-modern-4g", "inoi244_defconfig"),
        ):
            source = uboot_inputs.root / "targets" / target / "uboot" / defconfig
            source.parent.mkdir(parents=True)
            source.write_text(base)
            target_config = {
                "platform": "ums9117",
                "identity": {
                    "display_name": "Example Phone",
                    "compatible": "example,phone",
                },
                "profile": "microsd-uboot",
                "layout": {
                    **uboot_inputs.layout,
                    "ram_base": 0x80000000,
                    "ram_size": 0x04000000,
                    "timer_hz": 1000,
                    "kernel_load": 0x82000000,
                    "kernel_entry": 0x82000000,
                    "kernel_size": 0x01200000,
                    "fdt_load": 0x83E00000,
                    "fdt_size": 0x00010000,
                    "framebuffer": 0x83F00000,
                    "framebuffer_size": 0x00100000,
                    "fit_size": 0x00C00000,
                    "resident_start": 0x80100000,
                    "resident_limit": 0x81000000,
                },
                "uboot": {
                    **uboot_inputs.config,
                    "defconfig": f"uboot/{defconfig}",
                    "patches": [],
                    "copies": [{"source": "build-log.sh", "destination": "scripts/build-log.sh"}],
                },
            }
            with (
                mock.patch.object(common, "ROOT", uboot_inputs.root),
                mock.patch.object(sources_build, "fetch", return_value=uboot_inputs.archive),
                mock.patch.dict(os.environ, {"FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE": "a" * 64}),
            ):
                built = storage_build.build_profile_uboot(
                    target, target_config, uboot_inputs.work, 1
                )
                if built is None:
                    pytest.fail("configured U-Boot build did not produce an artifact")
                assert (built.dtb.read_bytes().strip()) == (f'"{target}"'.encode("ascii"))
                receipts.append(built.receipt["recipe"])
        assert (len(set(receipts))) == (3)

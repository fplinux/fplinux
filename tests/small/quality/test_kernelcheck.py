# SPDX-License-Identifier: GPL-2.0-only
"""Small component tests for kernel-check selection and work isolation.

External analyzers are replaced at their command boundary. These tests do not run
Clang, checkpatch, Kbuild, dtbs_check, Sparse, or a cross-compiler.
"""

from __future__ import annotations

import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli.build.kernel.state import PreparedLinuxState
from fplinux_cli.manifests.kernel import kconfig_values
from fplinux_cli.quality.kernel import analysis as kernel_analysis
from fplinux_cli.quality.kernel import contexts as kernel_contexts
from fplinux_cli.quality.kernel import runtime as kernel_runtime

if TYPE_CHECKING:
    from collections.abc import Iterator


def skip_linux_source_tools(
    _inputs: object, _archive: Path, _source: dict[str, Any], output: Path
) -> tuple[Path, ...]:
    """Replace patch/LLVM subprocesses in analyzer work-isolation scenarios."""
    output.write_text("")
    return ()


def apply_scripts_config(arguments: list[str]) -> None:
    """Edit a .config as Linux ``scripts/config --file F --enable A --disable B`` does."""
    option, path, *actions = arguments
    if option != "--file":
        message = f"scripts/config stand-in requires --file first: {arguments}"
        raise AssertionError(message)
    config = Path(path)
    lines = config.read_text().splitlines()
    settings = {"--enable": "CONFIG_{}=y", "--disable": "# CONFIG_{} is not set"}
    for action, symbol in zip(actions[::2], actions[1::2], strict=True):
        name = f"CONFIG_{symbol}"
        lines = [line for line in lines if not line.startswith((f"{name}=", f"# {name} "))]
        lines.append(settings[action].format(symbol))
    config.write_text("\n".join(lines) + "\n")


def resolve_initramfs_compression(config: Path) -> None:
    """Model removal of initial RAM disks and the embedded-archive compression choice."""
    values = kconfig_values(config.read_text())
    if values.get("CONFIG_BLK_DEV_INITRD") == "n":
        for symbol in (
            "CONFIG_INITRAMFS_SOURCE",
            "CONFIG_INITRAMFS_COMPRESSION_XZ",
            "CONFIG_INITRAMFS_COMPRESSION_GZIP",
            "CONFIG_RD_GZIP",
            "CONFIG_RD_XZ",
        ):
            values.pop(symbol, None)
        config.write_text("".join(f"{symbol}={value}\n" for symbol, value in values.items()))
        return
    if not values.get("CONFIG_INITRAMFS_SOURCE", '""').strip('"'):
        config.write_text(
            config.read_text().replace(
                "CONFIG_INITRAMFS_COMPRESSION_XZ=y\n",
                "CONFIG_INITRAMFS_COMPRESSION_GZIP=y\n",
            )
        )


def run_kconfig_tool(command: list[str]) -> None:
    """Apply the .config effects of scripts/config and olddefconfig; ignore other commands."""
    if Path(command[0]).name == "config":
        apply_scripts_config(command[1:])
    elif "olddefconfig" in command:
        output = next(Path(arg.removeprefix("O=")) for arg in command if arg.startswith("O="))
        resolve_initramfs_compression(output / ".config")


@dataclass
class AnalyzerContext:
    """Own analyzer inputs while replacing only external command boundaries."""

    cache: Path
    source: Path
    defconfig: Path
    fragment: Path
    projected: Path
    prepared_linux: PreparedLinuxState
    target_config: dict[str, Any]
    platform: dict[str, Any]
    target: str = "test-target"

    def run_check(self, target_config: dict[str, Any], profile: str | None = None) -> None:
        """Run one context check with Kconfig, analyzer and validator command stand-ins."""
        with (
            mock.patch.object(kernel_analysis, "CACHE", self.cache),
            mock.patch.object(kernel_contexts, "CACHE", self.cache),
            mock.patch.object(
                kernel_runtime, "load_sources", return_value={"linux": {"version": "fixture"}}
            ),
            mock.patch.object(kernel_contexts, "discover_targets", return_value=(self.target,)),
            mock.patch.object(
                kernel_contexts,
                "discover_profiles",
                return_value=("default", "microsd-uboot", "microsd-root"),
            ),
            mock.patch.object(
                kernel_analysis,
                "target_context",
                return_value=(target_config, self.platform, self.source, self.prepared_linux),
            ),
            mock.patch.object(
                kernel_analysis,
                "kernel_config_paths",
                return_value=(self.defconfig, self.fragment),
            ),
            mock.patch.object(kernel_analysis, "projected_sources", return_value=[self.projected]),
            mock.patch.object(
                kernel_analysis,
                "sparse_targets",
                return_value=["drivers/test-a.o", "drivers/test-b.o"],
            ),
            mock.patch(
                "fplinux_cli.quality.kernel.analysis.linux_state.require_prepared_linux",
                return_value=self.prepared_linux,
            ),
            mock.patch.object(kernel_analysis, "run", side_effect=run_kconfig_tool),
            mock.patch.object(
                kernel_analysis,
                "capture_text",
                return_value=subprocess.CompletedProcess(
                    [], 0, "drivers/test-a.o\ndrivers/test-b.o\n", ""
                ),
            ),
            mock.patch.object(kernel_analysis, "run_checkpatch"),
            mock.patch.object(
                kernel_analysis,
                "check_linux_changes",
                side_effect=skip_linux_source_tools,
            ),
            mock.patch.object(kernel_analysis, "run_dtbs_check", return_value=""),
            mock.patch.object(kernel_analysis, "verify_target_identity"),
        ):
            kernel_runtime.check_contexts(None, profile)

    def cache_path(self, name: str) -> Path:
        """Return one path inside this test's fixed Sparse cache directory."""
        return self.cache / "analysis" / "sparse" / self.target / "builds" / "release" / name


@pytest.fixture
def analyzer_context() -> Iterator[AnalyzerContext]:
    """Create a projected Linux context and promptly discard its analyzer work."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "linux"
        (source / "scripts").mkdir(parents=True)
        (source / ".clang-format").write_text("BasedOnStyle: LLVM\n")
        (source / "scripts/checkpatch.pl").write_text("#!/usr/bin/env perl\n")
        (source / "scripts/Makefile.build").write_text("# Replaced at the command boundary.\n")
        defconfig = root / "defconfig"
        defconfig.write_text("CONFIG_TEST=y\n")
        fragment = root / "fragment"
        fragment.write_text("CONFIG_BOARD=y\n")
        projected = root / "driver.c"
        projected.write_text("int test_driver;\n")
        target_config: dict[str, Any] = {
            "bootstrap": {"initramfs_delivery": "embedded"},
            "identity": {
                "display_name": "Test target",
                "compatible": "test,target",
            },
            "linux": {
                "dtb": "test-target.dtb",
                "patches": [],
                "copies": [],
                "appends": [],
                "root": {"kind": "initramfs"},
            },
        }
        platform: dict[str, Any] = {
            "identity": {"compatible": "test,soc"},
            "linux": {
                "arch": "arm",
                "analysis_cross_compile": "arm-linux-gnueabihf-",
                "config_script": "scripts/config",
                "dtb_output_directory": "arch/arm/boot/dts",
                "patches": [],
                "copies": [],
                "appends": [],
                "source_lock": "linux",
            },
        }
        yield AnalyzerContext(
            cache=root / ".cache",
            source=source,
            defconfig=defconfig,
            fragment=fragment,
            projected=projected,
            prepared_linux=PreparedLinuxState("a" * 64, "b" * 64),
            target_config=target_config,
            platform=platform,
        )


class KernelAnalyzerWorkIsolationTests:
    """Check disposable analyzer work while replacing external commands with stubs."""

    def test_each_invocation_discards_stale_analyzer_work(
        self, analyzer_context: AnalyzerContext
    ) -> None:
        """A new check cannot inherit object files from an earlier analyzer run."""
        analyzer_context.run_check(analyzer_context.target_config)
        stale = analyzer_context.cache_path("work") / "drivers" / "test-a.o"
        stale.parent.mkdir(parents=True)
        stale.write_text("stale object\n")

        analyzer_context.run_check(analyzer_context.target_config)
        assert not (stale.exists())

    def test_initramfs_preparation_keeps_compressor_with_modeled_kconfig(
        self, analyzer_context: AnalyzerContext
    ) -> None:
        """A temporary source lets the modeled Kconfig choice retain requested XZ."""
        analyzer_context.defconfig.write_text(
            'CONFIG_TEST=y\nCONFIG_INITRAMFS_SOURCE=""\nCONFIG_INITRAMFS_COMPRESSION_XZ=y\n'
        )

        analyzer_context.run_check(analyzer_context.target_config)

        config = kconfig_values((analyzer_context.cache_path("work") / ".config").read_text())
        assert (config["CONFIG_INITRAMFS_COMPRESSION_XZ"]) == ("y")
        source = Path(config["CONFIG_INITRAMFS_SOURCE"].strip('"'))
        assert source.is_dir()
        assert (list(source.iterdir())) == ([])

    def test_initramfs_analysis_writes_bootargs_for_the_checked_tree(
        self, analyzer_context: AnalyzerContext
    ) -> None:
        """The prepared analyzer context carries the RAM-root boot arguments."""
        analyzer_context.run_check(analyzer_context.target_config)

        root_include = (
            analyzer_context.cache_path("work") / "include/generated/fplinux/fplinux-root.dtsi"
        )
        assert ("init=/init rdinit=/init") in (root_include.read_text())

    def test_external_ram_analysis_keeps_ram_bootargs_without_embedding_an_archive(
        self, analyzer_context: AnalyzerContext
    ) -> None:
        """A separately loaded boot archive does not select Kbuild's compression choice."""
        analyzer_context.defconfig.write_text(
            'CONFIG_TEST=y\nCONFIG_INITRAMFS_SOURCE="old.cpio"\n'
            "CONFIG_INITRAMFS_COMPRESSION_XZ=y\n"
        )
        analyzer_context.target_config["bootstrap"]["initramfs_delivery"] = "external"

        analyzer_context.run_check(analyzer_context.target_config)

        output = analyzer_context.cache_path("work")
        config = kconfig_values((output / ".config").read_text())
        assert config["CONFIG_INITRAMFS_SOURCE"] == '""'
        assert not (output / "initramfs").exists()
        assert (
            "init=/init rdinit=/init"
            in (output / "include/generated/fplinux/fplinux-root.dtsi").read_text()
        )

    def test_external_root_accepts_removed_initrd_settings_after_modeled_kconfig(
        self, analyzer_context: AnalyzerContext
    ) -> None:
        """Disabling initial RAM disks removes their inherited source and decoders."""
        (analyzer_context.source / "scripts/config").write_text("#!/bin/sh\n")
        analyzer_context.defconfig.write_text(
            'CONFIG_TEST=y\nCONFIG_BLK_DEV_INITRD=y\nCONFIG_INITRAMFS_SOURCE=""\n'
            "CONFIG_INITRAMFS_COMPRESSION_XZ=y\nCONFIG_RD_GZIP=y\nCONFIG_RD_XZ=y\n"
        )
        config = analyzer_context.target_config
        config["bootstrap"]["initramfs_delivery"] = "none"
        config["linux"].update(
            root={
                "kind": "external",
                "filesystem": "ext4",
                "partuuid": "46504c52-02",
                "wait_seconds": 10,
            },
            config_disable=["CONFIG_BLK_DEV_INITRD"],
        )
        # This scenario observes Kconfig policy; DTB validation has separate tests.
        with mock.patch.object(kernel_analysis, "verify_root_bootargs"):
            analyzer_context.run_check(config, "microsd-root")

        output = (
            analyzer_context.cache
            / "analysis/sparse/test-target/profiles/microsd-root/builds/release/work"
        )
        actual = kconfig_values((output / ".config").read_text())
        assert actual == {"CONFIG_TEST": "y", "CONFIG_BOARD": "y", "CONFIG_BLK_DEV_INITRD": "n"}
        assert not (output / "initramfs").exists()

    def test_profile_actions_reach_configuration_through_modeled_kconfig(
        self, analyzer_context: AnalyzerContext
    ) -> None:
        """Configuration preparation applies profile actions through a Kconfig stand-in."""
        (analyzer_context.source / "scripts/config").write_text("#!/bin/sh\n")
        analyzer_context.defconfig.write_text("CONFIG_TEST=y\nCONFIG_PROFILE_DISABLED=y\n")
        linux = {
            **analyzer_context.target_config["linux"],
            "config_enable": ["CONFIG_PROFILE_ENABLED"],
            "config_disable": ["CONFIG_PROFILE_DISABLED"],
        }

        analyzer_context.run_check(
            {**analyzer_context.target_config, "linux": linux}, "microsd-uboot"
        )

        output = (
            analyzer_context.cache
            / "analysis/sparse/test-target/profiles/microsd-uboot/builds/release/work"
        )
        config = (output / ".config").read_text().splitlines()
        assert ("CONFIG_PROFILE_ENABLED=y") in (config)
        assert ("# CONFIG_PROFILE_DISABLED is not set") in (config)


class KernelProfileSelectionTests:
    """Keep default and explicitly named kernel contexts separate."""

    def test_each_global_profile_selects_every_board_offering_it(self) -> None:
        """The default alias preserves identity; microSD drops only a RAM-only board."""
        offered = {
            "first": ("default", "microsd-uboot"),
            "ram-only": ("default",),
            "second": ("default", "microsd-uboot"),
        }
        with (
            mock.patch.object(kernel_contexts, "discover_targets", return_value=tuple(offered)),
            mock.patch.object(
                kernel_contexts, "discover_profiles", side_effect=offered.__getitem__
            ),
        ):
            assert (kernel_contexts.target_profiles()) == (
                (("first", None), ("ram-only", None), ("second", None))
            )
            assert (kernel_contexts.target_profiles("default")) == (
                (("first", None), ("ram-only", None), ("second", None))
            )
            assert (kernel_contexts.target_profiles("microsd-uboot")) == (
                (("first", "microsd-uboot"), ("second", "microsd-uboot"))
            )

    def test_unknown_profile_is_rejected_by_context_selection(self) -> None:
        """Context selection refuses a name that is not a global boot profile."""
        with pytest.raises(SystemExit, match="unknown profile"):
            kernel_contexts.target_profiles("missing")


class KernelContextDispatchTests:
    """Reject an invalid worker limit before analyzer work starts."""

    def test_nonpositive_jobs_fail_before_context_discovery(self) -> None:
        """An invalid worker limit cannot inspect contexts or start analyzers."""
        with (
            mock.patch.object(
                kernel_runtime,
                "target_profiles",
                side_effect=AssertionError("invalid jobs must fail first"),
            ),
            pytest.raises(SystemExit, match="jobs must be positive"),
        ):
            kernel_runtime.check_contexts(None, jobs=0)


class KernelBindingDiagnosticsTests:
    """Check policy over controlled external-validator and Kbuild reports."""

    @pytest.mark.parametrize(
        ("status", "stdout", "stderr", "examples", "fails"),
        [
            (0, "", "", "make: done\n", False),
            (1, "", "", "", True),
            (0, "fixture.yaml: properties: Unresolvable reference\n", "", "", True),
            (0, "", "fixture.yaml: Missing additionalProperties\n", "", True),
            (0, "", "", "fixture.example.dtb: example@0: reg: invalid\n", True),
            (0, "", "", "fixture.example.dts:7.3: Warning (unit_address_vs_reg): node@0\n", True),
        ],
        ids=(
            "success",
            "tool-failure",
            "stdout-reference",
            "stderr-properties",
            "example-error",
            "example-warning",
        ),
    )
    def test_diagnostic_output_cannot_pass_even_with_zero_tool_status(
        self, status: int, stdout: str, stderr: str, examples: str, *, fails: bool
    ) -> None:
        """Schema references and compiled example failures must remain visible failures."""
        result = subprocess.CompletedProcess(["dt-doc-validate"], status, stdout, stderr)
        with (
            mock.patch.object(kernel_analysis, "run"),
            mock.patch.object(kernel_analysis, "capture_text", return_value=result),
            mock.patch.object(kernel_analysis, "run_dtbs_check", return_value=examples),
        ):
            if fails:
                with pytest.raises(SystemExit, match=r"binding .* findings"):
                    kernel_analysis.check_bindings(
                        Path("linux"), ["make"], ("fixture.yaml",), "fixture"
                    )
            else:
                kernel_analysis.check_bindings(
                    Path("linux"), ["make"], ("fixture.yaml",), "fixture"
                )

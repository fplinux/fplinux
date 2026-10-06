# SPDX-License-Identifier: GPL-2.0-only
"""Compile and validate generated board identity and output-owned profile data.

The small Makefile uses the platform's DTC flags without invoking Kbuild or
compiling any hardware description.
"""

from __future__ import annotations

import contextlib
import io
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

from fplinux_cli import common
from fplinux_cli.build.device_tree import (
    exact_path_properties,
    parse_nul_string,
    parse_nul_string_list,
)
from fplinux_cli.build.identity import (
    linux_identity_dtsi,
    linux_identity_dtsi_name,
    linux_machine_binding,
    linux_machine_binding_path,
)
from fplinux_cli.build.kernel.state import write_profile_root
from fplinux_cli.cli import target_new
from fplinux_cli.manifests import platforms, targets

from tests import ROOT
from tests.fdt import binary_tree
from tests.process import run_process


class NewTargetDeviceTreeTests(unittest.TestCase):
    """Compile the generated target's identity with minimal platform include stubs."""

    def test_new_target_compiles_with_its_own_board_identity(self) -> None:
        """CPP and DTC resolve the generated include and preserve the requested identity."""
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            platform_directory = root / "platforms/ums9117"
            (platform_directory / "host").mkdir(parents=True)
            shutil.copy2(ROOT / "platforms/ums9117/platform.toml", platform_directory)
            shutil.copy2(
                ROOT / "platforms/ums9117/host/target_skeleton.py", platform_directory / "host"
            )
            shutil.copytree(
                ROOT / "platforms/ums9117/target-template", platform_directory / "target-template"
            )
            shutil.copytree(ROOT / "profiles", root / "profiles")
            (root / "targets").mkdir()
            with (
                mock.patch.object(common, "ROOT", root),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                target_new.create_target(
                    "hammer-horizon-lte",
                    platform=None,
                    brand="HAMMER",
                    product="Horizon LTE",
                    compatible=None,
                )
                config = targets.load_target("hammer-horizon-lte")
                platform = platforms.load_platform(config["platform"])

            source = root / "targets/hammer-horizon-lte/linux/dts"
            (source / linux_identity_dtsi_name("hammer-horizon-lte")).write_bytes(
                linux_identity_dtsi(config["identity"], platform["identity"])
            )
            # These stubs replace SoC descriptions; no hardware configuration is validated.
            (source / "dt-bindings/leds").mkdir(parents=True)
            for relative in (
                "dt-bindings/leds/common.h",
                "ums9117-sc2720.dtsi",
                "ums9117-nand-readonly.dtsi",
                "ums9117-ram-session.dtsi",
            ):
                (source / relative).write_text("", encoding="utf-8")
            (source / "ums9117.dtsi").write_text(
                "/ { #address-cells = <1>; #size-cells = <1>;\n"
                "codec_dma: codec-dma {}; sc2720_fgu: fuel-gauge {};\n"
                "sc2720_kpled: keypad-light {}; usb: usb {}; };\n",
                encoding="utf-8",
            )
            preprocessed = root / "board.dts"
            dtb = root / "board.dtb"
            run_process(
                [
                    "cpp",
                    "-nostdinc",
                    "-undef",
                    "-D__DTS__",
                    "-x",
                    "assembler-with-cpp",
                    "-I",
                    str(source),
                    str(source / "ums9117-hammer-horizon-lte.dts"),
                    "-o",
                    str(preprocessed),
                ],
                name="preprocess generated target identity",
                timeout=30,
                check=True,
            )
            run_process(
                ["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb), str(preprocessed)],
                name="compile generated target identity",
                timeout=30,
                check=True,
            )
            properties = exact_path_properties(dtb.read_bytes(), ("/",))["/"]
            self.assertEqual(parse_nul_string(properties["model"], "model"), "HAMMER Horizon LTE")
            self.assertEqual(
                parse_nul_string_list(properties["compatible"], "compatible"),
                ("hammer,horizon-lte", "sprd,ums9117"),
            )


class LinuxMachineBindingTests(unittest.TestCase):
    """Validate generated board schemas together through real dtschema tools."""

    def test_boards_sharing_a_soc_validate_only_their_own_identity(self) -> None:
        """A shared fallback produces no duplicate schema or wrong-board validation."""
        identities = (
            {"display_name": "First phone", "compatible": "test,first"},
            {"display_name": "Second phone", "compatible": "test,second"},
        )
        platform = {"display_name": "Test SoC", "compatible": "test,soc"}
        with tempfile.TemporaryDirectory() as name:
            work = Path(name)
            bindings = []
            for identity in identities:
                path = work / linux_machine_binding_path(identity, arch="arm")
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(linux_machine_binding(identity, platform, arch="arm"))
                bindings.append(str(path))
            processed = work / "processed-schema.json"
            for command in (
                ["dt-doc-validate", *bindings],
                ["dt-mk-schema", "-j", "-o", str(processed), *bindings],
            ):
                result = run_process(
                    command,
                    name="validate generated board schemas",
                    timeout=30,
                    check=True,
                )
                self.assertEqual(result.stdout + result.stderr, "")

            for compatible, model, expected_model in (
                ("test,first", "First phone", None),
                ("test,second", "Second phone", None),
                ("test,first", "Wrong model", "First phone"),
                ("test,second", "Wrong model", "Second phone"),
            ):
                with self.subTest(compatible=compatible, model=model):
                    dtb = work / "board.dtb"
                    dtb.write_bytes(
                        binary_tree(
                            [
                                ("compatible", f"{compatible}\0test,soc\0".encode()),
                                ("model", f"{model}\0".encode()),
                                ("#address-cells", b"\0\0\0\1"),
                                ("#size-cells", b"\0\0\0\1"),
                            ],
                            [],
                        )
                    )
                    result = run_process(
                        ["dt-validate", "-s", str(processed), str(dtb)],
                        name="validate binary board identity",
                        timeout=30,
                        check=True,
                    )
                    diagnostics = result.stdout + result.stderr
                    if expected_model is None:
                        self.assertEqual(diagnostics, "")
                    else:
                        self.assertIn("model", diagnostics)
                        self.assertIn(expected_model, diagnostics)


class LinuxDeviceTreeProfileTests(unittest.TestCase):
    """Keep multiple DT identities and root profiles separate in compiled output."""

    def test_boards_and_profiles_compile_from_unchanged_shared_sources(self) -> None:
        """A shared source directory supports RAM, microSD and another board's DTB."""
        identities = {
            "first": {"display_name": "First phone", "compatible": "test,first"},
            "second": {"display_name": "Second phone", "compatible": "test,second"},
        }
        platform = {"display_name": "Test SoC", "compatible": "test,soc"}
        ram: dict[str, Any] = {"linux": {"root": {"kind": "initramfs"}}}
        microsd: dict[str, Any] = {
            "linux": {
                "root": {
                    "kind": "external",
                    "partuuid": "46504c58-02",
                    "filesystem": "ext4",
                    "wait_seconds": 10,
                }
            }
        }
        with tempfile.TemporaryDirectory() as name:
            work = Path(name)
            source = work / "source"
            source.mkdir()
            for target, identity in identities.items():
                (source / linux_identity_dtsi_name(target)).write_bytes(
                    linux_identity_dtsi(identity, platform)
                )
            (source / "first.dts").write_text(
                '/dts-v1/;\n#include "fplinux-first-identity.dtsi"\n#include "common.dtsi"\n'
            )
            (source / "second.dts").write_text(
                '/dts-v1/;\n#include "fplinux-second-identity.dtsi"\n#include "common.dtsi"\n'
            )
            (source / "common.dtsi").write_text(
                '/ { chosen { /include/ "fplinux-root.dtsi" }; };\n'
            )
            makefile = work / "Makefile"
            makefile.write_text(
                "DTC_FLAGS := -Wno-unit_address_vs_reg\n"
                f"include {ROOT / 'platforms/ums9117/linux/dts/unisoc.Makefile'}\n"
                ".PHONY: all\n"
                "all:\n"
                "\tcpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp "
                "-I $(src) $(src)/$(board).dts -o $(objtree)/board.dts\n"
                "\tdtc $(DTC_FLAGS) -I dts -O dtb "
                "-o $(objtree)/board.dtb $(objtree)/board.dts\n"
            )
            before = {path.name: path.read_bytes() for path in source.iterdir()}
            for path in source.iterdir():
                path.chmod(0o444)
            source.chmod(0o555)
            outputs: dict[tuple[str, str], bytes] = {}
            try:
                for target, profile, config in (
                    ("first", "default", ram),
                    ("first", "microsd-uboot", microsd),
                    ("second", "default", ram),
                    ("first", "default", ram),
                ):
                    with self.subTest(target=target, profile=profile):
                        output = work / "outputs" / target / profile
                        write_profile_root(output, config)
                        run_process(
                            [
                                "make",
                                "--no-print-directory",
                                "-f",
                                str(makefile),
                                f"src={source}",
                                f"objtree={output}",
                                f"board={target}",
                            ],
                            name="compile profile DTB with host dtc",
                            timeout=30,
                            check=True,
                        )
                        dtb = (output / "board.dtb").read_bytes()
                        key = (target, profile)
                        if key in outputs:
                            self.assertEqual(dtb, outputs[key])
                        outputs[key] = dtb
                self.assertEqual(
                    {path.name: path.read_bytes() for path in source.iterdir()}, before
                )
            finally:
                source.chmod(0o755)
                for path in source.iterdir():
                    path.chmod(0o644)

            for (target, profile), dtb in outputs.items():
                with self.subTest(target=target, profile=profile):
                    nodes = exact_path_properties(dtb, ("/", "/chosen"))
                    self.assertEqual(
                        parse_nul_string(nodes["/"]["model"], "model"),
                        "First phone" if target == "first" else "Second phone",
                    )
                    self.assertEqual(
                        parse_nul_string_list(nodes["/"]["compatible"], "compatible"),
                        ("test,first", "test,soc")
                        if target == "first"
                        else ("test,second", "test,soc"),
                    )
                    bootargs = parse_nul_string(nodes["/chosen"]["bootargs"], "bootargs").split()
                    if profile == "default":
                        self.assertIn("rdinit=/init", bootargs)
                        self.assertIn("init=/init", bootargs)
                        self.assertFalse(any(arg.startswith("root=") for arg in bootargs))
                    else:
                        for argument in (
                            "root=PARTUUID=46504c58-02",
                            "rootfstype=ext4",
                            "rootwait=10",
                            "rw",
                            "init=/sbin/init",
                        ):
                            self.assertIn(argument, bootargs)
                        self.assertFalse(any(arg.startswith("rdinit=") for arg in bootargs))


if __name__ == "__main__":
    unittest.main()

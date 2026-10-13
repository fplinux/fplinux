# SPDX-License-Identifier: GPL-2.0-only
"""Compile DT projections with host tools and controlled external inputs."""

from __future__ import annotations

import contextlib
import io
import shutil
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.build.device_tree import (
    exact_path_properties,
    parse_nul_string,
    parse_nul_string_list,
    verify_profile_dtb_layout,
)
from fplinux_cli.build.identity import (
    linux_audio_policy_dtsi,
    linux_identity_dtsi,
    linux_identity_dtsi_name,
    linux_machine_binding,
    linux_machine_binding_path,
)
from fplinux_cli.build.kernel.prepare import generated_linux_files, integration_inputs
from fplinux_cli.build.kernel.state import write_profile_root
from fplinux_cli.build.sources import copy_steps
from fplinux_cli.cli import target_new
from fplinux_cli.manifests import platforms, targets

from tests import ROOT
from tests.fdt import binary_tree
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator

TREE_FIXTURES = ROOT / "tests/host_tool/build/fixtures/linux-device-tree"


@pytest.mark.parametrize(
    ("field", "property_name"),
    [
        ("speaker_vibration", "fplinux,speaker-vibration"),
        ("speaker_from_headphones", "fplinux,headphone-fed-speaker"),
        ("headfree_headphones_only", "fplinux,headfree-headphones-only"),
    ],
)
def test_compiled_audio_policy_contains_only_the_selected_topology_property(
    tmp_path: Path, field: str, property_name: str
) -> None:
    """Each policy choice becomes the expected boolean property in a minimal audio node."""
    policy = {
        "speaker_vibration": False,
        "speaker_from_headphones": False,
        "headfree_headphones_only": False,
    }
    policy[field] = True
    (tmp_path / "audio-policy.dtsi").write_bytes(linux_audio_policy_dtsi(policy))
    source = tmp_path / "audio.dts"
    source.write_bytes((TREE_FIXTURES / "audio.dts.fixture").read_bytes())
    dtb = tmp_path / "audio.dtb"
    run_process(
        ["dtc", "-I", "dts", "-O", "dtb", "-i", str(tmp_path), "-o", str(dtb), str(source)],
        name="compile generated audio topology",
        timeout=15,
        check=True,
    )
    assert exact_path_properties(dtb, "/audio")["/audio"] == {property_name: b""}


def _write_peripheral_binding_stubs(include: Path) -> None:
    """Supply symbols for CPP and DTC without validating peripheral configuration."""
    event_codes = [
        *(f"KEY_NUMERIC_{digit}" for digit in range(10)),
        "KEY_NUMERIC_STAR",
        "KEY_NUMERIC_POUND",
        "KEY_UP",
        "KEY_DOWN",
        "KEY_LEFT",
        "KEY_RIGHT",
        "KEY_OK",
        "KEY_F13",
        "KEY_F14",
        "KEY_PICKUP_PHONE",
        "KEY_POWER",
    ]
    bindings = {
        "interrupt-controller/arm-gic.h": "#define GIC_SPI 0\n#define IRQ_TYPE_LEVEL_HIGH 4\n",
        "gpio/gpio.h": "#define GPIO_ACTIVE_HIGH 0\n#define GPIO_ACTIVE_LOW 1\n",
        "input/input.h": "#define MATRIX_KEY(row, col, key) (key)\n",
        "input/linux-event-codes.h": "".join(
            f"#define {name} {value}\n" for value, name in enumerate(event_codes)
        ),
        "leds/common.h": '#define LED_FUNCTION_KBD_BACKLIGHT "kbd_backlight"\n',
    }
    for relative, contents in bindings.items():
        header = include / "dt-bindings" / relative
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(contents, encoding="ascii")


@pytest.fixture
def nokia110_microsd_dtb(tmp_path: Path) -> Path:
    """Compile the board's DT projections; binding stubs do not validate peripherals."""
    target = "nokia-110-4g"
    config = targets.load_target(target, "microsd-uboot")
    platform = platforms.load_platform(config["platform"])
    source = tmp_path / "source"
    dts_directory = platform["linux"]["dts_directory"]
    projections = [
        (path, destination)
        for operation, _identity, destination, path in integration_inputs(target, config, platform)
        if operation.endswith("copy")
        and destination.startswith((f"{dts_directory}/", "include/dt-bindings/"))
    ]
    copy_steps(source, projections)
    for destination, contents in generated_linux_files(target, config, platform).items():
        if destination.startswith(f"{dts_directory}/"):
            (source / destination).write_bytes(contents)
    _write_peripheral_binding_stubs(source / "include")

    output = tmp_path / "output"
    write_profile_root(output, config)
    board = source / "arch/arm/boot/dts" / Path(config["linux"]["dtb"]).with_suffix(".dts")
    preprocessed = tmp_path / "board.dts"
    dtb = tmp_path / "board.dtb"
    run_process(
        [
            "cpp",
            "-nostdinc",
            "-undef",
            "-D__DTS__",
            "-x",
            "assembler-with-cpp",
            "-I",
            str(source / "include"),
            "-I",
            str(source / dts_directory),
            str(board),
            "-o",
            str(preprocessed),
        ],
        name="preprocess Nokia 110 microSD DT projections",
        timeout=30,
        check=True,
    )
    run_process(
        [
            "dtc",
            "-I",
            "dts",
            "-O",
            "dtb",
            "-i",
            str(output / "include/generated/fplinux"),
            "-o",
            str(dtb),
            str(preprocessed),
        ],
        name="compile Nokia 110 microSD DT projections",
        timeout=30,
        check=True,
    )
    return dtb


def test_nokia110_compiled_microsd_tree_preserves_the_boot_display_reservation(
    nokia110_microsd_dtb: Path,
) -> None:
    """The board DTB keeps Linux RAM and scanout memory compatible with the SD handoff."""
    layout = {
        "ram_base": 0x80000000,
        "ram_size": 0x04000000,
        "fdt_load": 0x83E00000,
        "fdt_size": 0x00010000,
        "fdt_pad": 0x00003000,
        "framebuffer": 0x83F00000,
        "framebuffer_size": 0x00100000,
    }
    verify_profile_dtb_layout(
        nokia110_microsd_dtb, layout, {"base": 0x80200000, "size": 0x03C00000}
    )
    nodes = exact_path_properties(
        nokia110_microsd_dtb, ("/memory@80200000", "/reserved-memory/framebuffer@83f00000")
    )
    assert nodes["/memory@80200000"]["reg"] == b"\x80\x20\x00\x00\x03\xc0\x00\x00"
    reservation = nodes["/reserved-memory/framebuffer@83f00000"]
    assert reservation["reg"] == b"\x83\xf0\x00\x00\x00\x10\x00\x00"
    assert reservation["no-map"] == b""


class NewTargetDeviceTreeTests:
    """Compile the generated target's identity with minimal platform include stubs."""

    def test_new_target_compiles_with_its_own_board_identity(self) -> None:
        """CPP and DTC resolve the generated include and preserve the requested identity."""
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            platform_directory = root / "platforms/ums9117"
            (platform_directory / "host").mkdir(parents=True)
            (platform_directory / "linux/overlay").mkdir(parents=True)
            shutil.copy2(ROOT / "platforms/ums9117/platform.toml", platform_directory)
            shutil.copy2(
                ROOT / "platforms/ums9117/host/target_skeleton.py", platform_directory / "host"
            )
            shutil.copytree(
                ROOT / "platforms/ums9117/target-template", platform_directory / "target-template"
            )
            shutil.copytree(ROOT / "platforms/ums9117/loader", platform_directory / "loader")
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

            source = root / "targets/hammer-horizon-lte/linux/overlay/arch/arm/boot/dts/unisoc"
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
            (source / "ums9117.dtsi").write_bytes(
                (TREE_FIXTURES / "ums9117.dtsi.fixture").read_bytes()
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
            assert (parse_nul_string(properties["model"], "model")) == ("HAMMER Horizon LTE")
            assert (parse_nul_string_list(properties["compatible"], "compatible")) == (
                ("hammer,horizon-lte", "sprd,ums9117")
            )


class LinuxMachineBindingTests:
    """Validate generated board schemas together through real dtschema tools."""

    @pytest.fixture(scope="class")
    @classmethod
    def processed_schema(cls) -> Iterator[Path]:
        """Process both board bindings once for the class's identity cases."""
        identities = (
            {"display_name": "First phone", "compatible": "test,first"},
            {"display_name": "Second phone", "compatible": "test,second"},
        )
        platform = {"display_name": "Test SoC", "compatible": "test,soc"}
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
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
                assert result.stdout + result.stderr == ""
            yield processed

    @pytest.mark.parametrize(
        ("compatible", "model", "expected_model"),
        [
            pytest.param("test,first", "First phone", None, id="first-board"),
            pytest.param("test,second", "Second phone", None, id="second-board"),
            pytest.param("test,first", "Wrong model", "First phone", id="first-wrong-model"),
            pytest.param("test,second", "Wrong model", "Second phone", id="second-wrong-model"),
        ],
    )
    def test_boards_sharing_a_soc_validate_only_their_own_identity(
        self,
        processed_schema: Path,
        tmp_path: Path,
        compatible: str,
        model: str,
        expected_model: str | None,
    ) -> None:
        """A shared fallback produces no duplicate schema or wrong-board validation."""
        dtb = tmp_path / "board.dtb"
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
            ["dt-validate", "-s", str(processed_schema), str(dtb)],
            name="validate binary board identity",
            timeout=30,
            check=True,
        )
        diagnostics = result.stdout + result.stderr
        if expected_model is None:
            assert diagnostics == ""
        else:
            assert "model" in diagnostics
            assert expected_model in diagnostics


class LinuxDeviceTreeProfileTests:
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
            identity_tree = (TREE_FIXTURES / "identity.dts.fixture").read_text(encoding="ascii")
            for target in ("first", "second"):
                (source / f"{target}.dts").write_text(
                    identity_tree.format(target=target), encoding="ascii"
                )
            (source / "common.dtsi").write_text(
                '/ { chosen { /include/ "fplinux-root.dtsi" }; };\n'
            )
            makefile = work / "Makefile"
            makefile.write_text(
                (TREE_FIXTURES / "consumer.make.fixture")
                .read_text(encoding="ascii")
                .format(include=ROOT / "platforms/ums9117/linux/fragments/dts/unisoc.Makefile"),
                encoding="ascii",
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
                        assert (dtb) == (outputs[key])
                    outputs[key] = dtb
                assert ({path.name: path.read_bytes() for path in source.iterdir()}) == (before)
            finally:
                source.chmod(0o755)
                for path in source.iterdir():
                    path.chmod(0o644)

            for (target, profile), dtb in outputs.items():
                nodes = exact_path_properties(dtb, ("/", "/chosen"))
                assert (parse_nul_string(nodes["/"]["model"], "model")) == (
                    "First phone" if target == "first" else "Second phone"
                )
                assert (parse_nul_string_list(nodes["/"]["compatible"], "compatible")) == (
                    ("test,first", "test,soc")
                    if target == "first"
                    else ("test,second", "test,soc")
                )
                bootargs = parse_nul_string(nodes["/chosen"]["bootargs"], "bootargs").split()
                if profile == "default":
                    assert ("rdinit=/init") in (bootargs)
                    assert ("init=/init") in (bootargs)
                    assert not (any(arg.startswith("root=") for arg in bootargs))
                else:
                    for argument in (
                        "root=PARTUUID=46504c58-02",
                        "rootfstype=ext4",
                        "rootwait=10",
                        "rw",
                        "init=/sbin/init",
                    ):
                        assert (argument) in (bootargs)
                    assert not (any(arg.startswith("rdinit=") for arg in bootargs))

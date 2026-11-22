# SPDX-License-Identifier: GPL-2.0-only
"""Compile DT projections with host tools and controlled external inputs."""

from __future__ import annotations

import shutil
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    exact_path_properties,
    parse_nul_string,
    parse_nul_string_list,
    verify_dtb_kconfig,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    write_boot_dtb,
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


@pytest.mark.parametrize("profile", [None, "microsd-uboot"], ids=["ram", "card"])
def test_profile_boot_dtb_preserves_arguments_from_board(
    tmp_path: Path, profile: str | None
) -> None:
    """Board arguments in /chosen/bootargs survive profile specialization."""
    board_dtb = tmp_path / "base" / "itel.dtb"
    board_dtb.parent.mkdir()
    board_args = b"clk_ignore_unused regulator_ignore_unused\0"
    board_dtb.write_bytes(binary_tree([], [("chosen", [("bootargs", board_args)], [])]))
    target = targets.load_target("itel-neo-r60-plus", profile)

    # Exercise DTB stamping with fdtput independently of Kbuild.
    dtb = write_boot_dtb(board_dtb, tmp_path, target, "a" * 64)
    verify_root_bootargs(dtb, target["linux"]["root"])

    chosen = exact_path_properties(dtb, "/chosen")["/chosen"]
    bootargs = parse_nul_string(chosen["bootargs"], "bootargs").split()
    if profile is None:
        assert bootargs.count("rdinit=/init") == 1
    else:
        assert bootargs.count("root=PARTUUID=46504c58-02") == 1
        assert "rdinit=/init" not in bootargs
    assert "clk_ignore_unused" in bootargs
    assert "regulator_ignore_unused" in bootargs


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


@pytest.fixture
def dt_config_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Own two board manifests, with amplifier wiring declared by only one board."""
    for relative, source in (
        ("platforms/sc9820e/platform.toml", "tests/fixtures/sc9820e-manifest/platform.toml"),
        ("targets/phone/target.toml", "tests/fixtures/sc9820e-manifest/target.toml"),
        ("targets/other/target.toml", "tests/fixtures/sc9820e-manifest/target.toml"),
        ("profiles/default/profile.toml", "profiles/default/profile.toml"),
    ):
        destination = tmp_path / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes((ROOT / source).read_bytes())
    (tmp_path / "platforms/sc9820e/linux/overlay").mkdir(parents=True)
    for target in ("phone", "other"):
        (tmp_path / f"targets/{target}/linux/overlay").mkdir(parents=True)
        (tmp_path / f"targets/{target}/loader").mkdir()
    platform = tmp_path / "platforms/sc9820e/platform.toml"
    platform.write_text(
        platform.read_text(encoding="utf-8").replace(
            "dt_config_checks = []",
            'dt_config_checks = [{path="/controller", compatible="example,controller", '
            'config="CONFIG_CONTROLLER"}]',
        ),
        encoding="utf-8",
    )
    board = tmp_path / "targets/phone/target.toml"
    board.write_text(
        board.read_text(encoding="utf-8").replace(
            "[linux]\n",
            '[linux]\ndt_config_checks = [{path="/controller/amplifier", '
            'compatible="example,amplifier", config="CONFIG_AMPLIFIER"}]\n',
        ),
        encoding="utf-8",
    )
    monkeypatch.setattr(common, "ROOT", tmp_path)
    return tmp_path


@pytest.mark.parametrize(
    ("target", "missing_symbol", "expected_error"),
    [
        (
            "phone",
            "CONFIG_AMPLIFIER",
            (
                "enabled DTB node /controller/amplifier compatible example,amplifier "
                "requires CONFIG_AMPLIFIER=y"
            ),
        ),
        (
            "phone",
            "CONFIG_CONTROLLER",
            (
                "enabled DTB node /controller compatible example,controller "
                "requires CONFIG_CONTROLLER=y"
            ),
        ),
        ("other", "CONFIG_AMPLIFIER", None),
        (
            "other",
            "CONFIG_CONTROLLER",
            (
                "enabled DTB node /controller compatible example,controller "
                "requires CONFIG_CONTROLLER=y"
            ),
        ),
    ],
)
def test_compiled_dt_requires_platform_and_selected_board_config_owners(
    dt_config_checkout: Path, target: str, missing_symbol: str, expected_error: str | None
) -> None:
    """The board requirement rejects a missing driver without spreading to a sibling board."""
    dtb = dt_config_checkout / "owners.dtb"
    run_process(
        [
            "dtc",
            "-I",
            "dts",
            "-O",
            "dtb",
            "-o",
            str(dtb),
            str(TREE_FIXTURES / "config-owners.dts.fixture"),
        ],
        name="compile platform and board config owners",
        timeout=15,
        check=True,
    )
    checks = targets.load_target(target)["linux"]["dt_config_checks"]
    config = {"CONFIG_CONTROLLER": "y", "CONFIG_AMPLIFIER": "y"}
    verify_dtb_kconfig(dtb, config, checks)

    del config[missing_symbol]

    if expected_error is None:
        verify_dtb_kconfig(dtb, config, checks)
    else:
        with pytest.raises(DeviceTreeError) as failure:
            verify_dtb_kconfig(dtb, config, checks)
        assert str(failure.value) == expected_error


def _write_peripheral_binding_stubs(include: Path) -> None:
    """Supply symbols for CPP and DTC without validating peripheral configuration."""
    event_codes = {
        "KEY_NUMERIC_0": 0x200,
        "KEY_NUMERIC_1": 0x201,
        "KEY_NUMERIC_2": 0x202,
        "KEY_NUMERIC_3": 0x203,
        "KEY_NUMERIC_4": 0x204,
        "KEY_NUMERIC_5": 0x205,
        "KEY_NUMERIC_6": 0x206,
        "KEY_NUMERIC_7": 0x207,
        "KEY_NUMERIC_8": 0x208,
        "KEY_NUMERIC_9": 0x209,
        "KEY_NUMERIC_STAR": 0x20A,
        "KEY_NUMERIC_POUND": 0x20B,
        "KEY_UP": 103,
        "KEY_DOWN": 108,
        "KEY_LEFT": 105,
        "KEY_RIGHT": 106,
        "KEY_OK": 0x160,
        "KEY_F13": 183,
        "KEY_F14": 184,
        "KEY_PICKUP_PHONE": 0x1BD,
        "KEY_POWER": 116,
        "KEY_0": 11,
        "KEY_1": 2,
        "KEY_2": 3,
        "KEY_3": 4,
        "KEY_4": 5,
        "KEY_5": 6,
        "KEY_6": 7,
        "KEY_7": 8,
        "KEY_8": 9,
        "KEY_9": 10,
        "KEY_PHONE": 169,
        "KEY_ENTER": 28,
        "KEY_MENU": 139,
        "KEY_BACK": 158,
        "KEY_UNKNOWN": 240,
        "KEY_VOLUMEDOWN": 114,
        "KEY_VOLUMEUP": 115,
        "KEY_CAMERA": 212,
    }
    bindings = {
        "interrupt-controller/arm-gic.h": (
            "#define GIC_SPI 0\n#define GIC_PPI 1\n"
            "#define GIC_CPU_MASK_SIMPLE(cpus) 0\n"
            "#define IRQ_TYPE_LEVEL_HIGH 4\n#define IRQ_TYPE_LEVEL_LOW 8\n"
        ),
        "gpio/gpio.h": "#define GPIO_ACTIVE_HIGH 0\n#define GPIO_ACTIVE_LOW 1\n",
        "input/input.h": (
            "#include <dt-bindings/input/linux-event-codes.h>\n"
            "#define MATRIX_KEY(row, col, key) (((row) << 24) | ((col) << 16) | (key))\n"
        ),
        "input/linux-event-codes.h": "".join(
            f"#define {name} {value}\n" for name, value in event_codes.items()
        ),
        "leds/common.h": '#define LED_FUNCTION_KBD_BACKLIGHT "kbd_backlight"\n',
    }
    for relative, contents in bindings.items():
        header = include / "dt-bindings" / relative
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(contents, encoding="ascii")


def _compile_target_dtb(tmp_path: Path, target: str, *, profile: str | None = None) -> Path:
    """Compile real board projections with syntax-only peripheral binding stubs."""
    config = targets.load_target(target, profile)
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
        name=f"preprocess {target} {profile or 'default'} DT projections",
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
        name=f"compile {target} {profile or 'default'} DT projections",
        timeout=30,
        check=True,
    )
    return dtb


def test_bq_compiled_audio_supplies_external_speaker_waveform(tmp_path: Path) -> None:
    """The BQ audio node supplies its route and the exact active-high control delays."""
    dtb = _compile_target_dtb(tmp_path, "bq-2830l-tank-connect-max")
    path = "/soc/audio@40000000"
    audio = exact_path_properties(dtb, (path,))[path]
    assert audio["fplinux,headphone-fed-speaker"] == b""
    assert audio["fplinux,external-fm"] == b""
    assert audio["speaker-enable-gpios"][4:] == bytes.fromhex("00000072 00000000")
    assert audio["fplinux,speaker-enable-delays-us"] == b"\0\0\0\2" * 7
    assert audio["fplinux,speaker-off-delay-us"] == bytes.fromhex("000003e8")


@pytest.mark.parametrize(
    ("target", "expected_id", "expected_page_bytes"),
    [
        ("inoi-244-modern-4g", None, None),
        ("inoi-240-modern-4g", 0x21E5, 2112),
        ("nokia-ta1618", 0xB1A1, 2176),
    ],
)
def test_compiled_nand_policy_keeps_chip_discovery_and_fixed_boards_distinct(
    tmp_path: Path, target: str, expected_id: int | None, expected_page_bytes: int | None
) -> None:
    """INOI244 selects the fitted chip while existing fixed boards retain their guard."""
    dtb = _compile_target_dtb(tmp_path, target)
    path = "/soc/nand-controller@21a00000"
    properties = exact_path_properties(dtb, (path,))[path]
    assert properties["compatible"] == b"fplinux,ums9117-nandc-ro\0"
    expected_property = None if expected_id is None else expected_id.to_bytes(4, "big")
    assert properties.get("fplinux,nand-id") == expected_property
    declaration = targets.load_target(target)["nand"]
    assert declaration.get("id") == expected_id
    assert declaration.get("raw_page_bytes") == expected_page_bytes


def test_konrow_compiled_nand_uses_parallel_reader(tmp_path: Path) -> None:
    """The parallel reader owns the fixed ID and needs the full parallel pin range."""
    dtb = _compile_target_dtb(tmp_path, "konrow-senior-280-plus")
    path = "/soc/nand-controller@21a00000"
    properties = exact_path_properties(dtb, (path,))[path]

    assert properties["compatible"] == b"fplinux,ums9117-nandc-parallel-ro\0"
    assert "fplinux,nand-id" not in properties
    assert len(properties["sprd,pinconf-values"]) == 17 * 4


@pytest.fixture
def nokia110_microsd_dtb(tmp_path: Path) -> Path:
    """Compile the board's DT projections; binding stubs do not validate peripherals."""
    return _compile_target_dtb(tmp_path, "nokia-110-4g", profile="microsd-uboot")


def test_qin_compiled_ram_tree_has_canonical_initrd_markers_and_ram_bootargs(
    tmp_path: Path,
) -> None:
    """The real projection compiles with one root include owning both initrd cells."""
    dtb = _compile_target_dtb(tmp_path, "xiaomi-qin-1s-plus")
    nodes = exact_path_properties(dtb, ("/chosen", "/memory@80000000"))
    chosen = nodes["/chosen"]
    assert chosen["linux,initrd-start"] == bytes.fromhex("e5e5e5e5")
    assert chosen["linux,initrd-end"] == bytes.fromhex("f6f6f6f6")
    assert chosen["rng-seed"] == bytes([0xA1]) * 64
    bootargs = parse_nul_string(chosen["bootargs"], "bootargs").split()
    assert "init=/init" in bootargs
    assert "rdinit=/init" in bootargs
    assert not any(argument.startswith("root=") for argument in bootargs)
    memory = nodes["/memory@80000000"]
    assert memory["device_type"] == b"memory\0"
    assert memory["reg"] == bytes.fromhex("8000000020000000")


def test_qin_compiled_keymaps_publish_phone_role_codes(tmp_path: Path) -> None:
    """The board's matrix and GPIO keys deliver the phone application's Linux key ABI."""
    dtb = _compile_target_dtb(tmp_path, "xiaomi-qin-1s-plus")
    gpio_codes = {
        "key-0": "00000200",
        "key-1": "00000201",
        "key-2": "00000202",
        "key-3": "00000203",
        "key-5": "00000205",
        "key-7": "00000207",
        "key-8": "00000208",
        "key-9": "00000209",
        "key-hash": "0000020b",
        "key-star": "0000020a",
        "key-back": "000000b8",
        "key-power": "00000074",
    }
    paths = ("/keypad@40250000", "/gpio-keys", *(f"/gpio-keys/{key}" for key in gpio_codes))
    nodes = exact_path_properties(dtb, paths)
    assert nodes["/keypad@40250000"]["linux,keymap"] == bytes.fromhex(
        "000001bd 0001006a 00020069 01000067 01010160 0102006c 020000b7 02010204 02020206"
    )
    assert nodes["/gpio-keys"]["compatible"] == b"xiaomi,qin1s-plus-gpio-keys\0"
    for key, expected_code in gpio_codes.items():
        assert nodes[f"/gpio-keys/{key}"]["linux,code"] == bytes.fromhex(expected_code)


def test_konrow_compiled_keymap_uses_measured_keys_for_navigation(tmp_path: Path) -> None:
    """The physical green and side keys provide OK and arrows without phantom roles."""
    dtb = _compile_target_dtb(tmp_path, "konrow-senior-280-plus")
    path = "/soc/keypad@40250000"
    keymap = exact_path_properties(dtb, (path,))[path]["linux,keymap"]
    roles = {}
    for offset in range(0, len(keymap), 4):
        entry = int.from_bytes(keymap[offset : offset + 4], "big")
        code = entry & 0xFFFF
        if code in (0x0069, 0x006A, 0x0160, 0x01BD):
            roles[entry >> 16] = code
    assert roles == {0x0004: 0x006A, 0x0104: 0x0069, 0x0400: 0x0160}


def test_konrow_compiled_peripherals_enable_card_headphones_and_battery(tmp_path: Path) -> None:
    """The DT exposes a fixed-session card, headphone-only audio and calibrated FGU."""
    dtb = _compile_target_dtb(tmp_path, "konrow-senior-280-plus")
    nodes = exact_path_properties(
        dtb,
        (
            "/soc/mmc@20300000",
            "/soc/audio@40000000",
            "/soc/spi@40600000/pmic@0/fuel-gauge",
            "/soc/spi@40600000/pmic@0/keypad-backlight@df8",
        ),
    )
    card = nodes["/soc/mmc@20300000"]
    assert card["status"] == b"okay\0"
    assert card["non-removable"] == b""
    audio = nodes["/soc/audio@40000000"]
    assert audio["status"] == b"okay\0"
    assert audio["pinctrl-names"] == b"audio\0"
    assert audio["firmware-name"] == b"fplinux/konrow280plus-audio-profile.bin\0"
    assert audio["fplinux,headphones-only"] == b""
    assert "fplinux,microphone-capture" not in audio
    assert "speaker-enable-gpios" not in audio
    gauge = nodes["/soc/spi@40600000/pmic@0/fuel-gauge"]
    assert gauge["status"] == b"okay\0"
    assert gauge["fplinux,current-calibration"] == bytes.fromhex("0000000a 00000014")
    light = nodes["/soc/spi@40600000/pmic@0/keypad-backlight@df8"]
    assert light["status"] == b"okay\0"
    assert light["fplinux,current-code"] == bytes.fromhex("0000000d")


def test_uniwa_compiled_keymap_exposes_the_additional_volume_column(tmp_path: Path) -> None:
    """The compiled map assigns volume roles and preserves an unnamed key."""
    dtb = _compile_target_dtb(tmp_path, "uniwa-v1000-4g")
    path = "/soc/keypad@40250000"
    keymap = exact_path_properties(dtb, (path,))[path]["linux,keymap"]
    column = {}
    for offset in range(0, len(keymap), 4):
        entry = int.from_bytes(keymap[offset : offset + 4], "big")
        position = entry >> 16
        if position & 0xFF == 4:
            column[position] = entry & 0xFFFF
    assert column == {0x0004: 114, 0x0104: 240, 0x0204: 115, 0x0304: 115, 0x0404: 114}


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
    """Compile a generated board with its declared platform and target overlays."""

    def test_new_target_compiles_with_its_own_board_identity(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """The projected DTB carries the requested identity and required boot nodes."""
        root = tmp_path / "checkout"
        for relative in ("platforms/ums9117", "platforms/shared/linux", "include", "profiles"):
            shutil.copytree(ROOT / relative, root / relative)
        shutil.copy2(ROOT / "sources.lock.toml", root)
        (root / "targets").mkdir()
        monkeypatch.setattr(common, "ROOT", root)

        target = "hammer-horizon-lte"
        target_new.create_target(
            target,
            platform=None,
            brand="HAMMER",
            product="Horizon LTE",
            compatible=None,
        )
        dtb = _compile_target_dtb(tmp_path, target)

        usb_path = "/soc/usb@20200000"
        supply_path = "/soc/spi@40600000/pmic@0/regulators/LDO_USB33"
        nodes = exact_path_properties(dtb, ("/", usb_path, supply_path, "/fplinux-session"))
        usb = nodes[usb_path]
        assert parse_nul_string(usb["compatible"], "compatible") == "sprd,ums9117-musb"
        assert usb["vddusb-supply"] == nodes[supply_path]["phandle"]
        assert nodes["/fplinux-session"]["compatible"] == b"fplinux,ram-session\0"
        properties = nodes["/"]
        assert parse_nul_string(properties["model"], "model") == "HAMMER Horizon LTE"
        assert parse_nul_string_list(properties["compatible"], "compatible") == (
            "hammer,horizon-lte",
            "sprd,ums9117",
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

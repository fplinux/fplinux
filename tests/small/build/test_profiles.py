# SPDX-License-Identifier: GPL-2.0-only
"""Global boot-policy selection and board configuration composition."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import selection as alpine_state
from fplinux_cli.build.bootstrap.recipe import bootstrap_display, generated_bootstrap_files
from fplinux_cli.build.bootstrap.verify import verify_forbidden_dtb_markers
from fplinux_cli.build.kernel import configuration as kernel_build
from fplinux_cli.build.kernel import prepare as linux_build
from fplinux_cli.common import ROOT
from fplinux_cli.manifests import kernel, paths, platforms, targets

from tests.fdt import binary_tree

if TYPE_CHECKING:
    from pathlib import Path

PROFILE_FIXTURES = ROOT / "tests/fixtures/profile-config"
REPOSITORY_PROFILES = [
    pytest.param(target, profile, id=f"{target}-{profile}")
    for target in paths.discover_targets()
    for profile in paths.discover_profiles(target)
]


class ProfileContext:
    """Keep target features common while changing the system root."""

    def __init__(self, root: Path) -> None:
        """Provide two board manifests and real global boot-policy inputs."""
        self.root = root
        shutil.copyfile(PROFILE_FIXTURES / "sources.lock.toml", self.root / "sources.lock.toml")
        shutil.copytree(ROOT / "profiles", self.root / "profiles")
        self.platform = common.load_toml(PROFILE_FIXTURES / "platform.toml")
        shared = self.root / "platforms/demo"
        (shared / "kernel").mkdir(parents=True)
        (shared / "kernel/defconfig").write_text("CONFIG_FEATURE=y\nCONFIG_BOARD=1\n")
        (shared / "loader").mkdir()
        shutil.copyfile(PROFILE_FIXTURES / "assets.lock.toml", shared / "loader/assets.lock.toml")
        shutil.copyfile(PROFILE_FIXTURES / "uboot.lock.toml", shared / "uboot.lock.toml")
        (shared / "bootstrap-microsd").mkdir()
        (shared / "uboot").mkdir()
        (shared / "uboot/board_defconfig").write_text("CONFIG_ARM=y\n")
        for target in ("first", "second"):
            target_root = self.root / "targets" / target
            (target_root / "kernel").mkdir(parents=True)
            (target_root / "linux/overlay").mkdir(parents=True)
            (target_root / "kernel/config.fragment").write_text("CONFIG_BOARD=2\n")
            self.write_target_manifest(target)

    def write_target_manifest(self, target: str, fixture: str = "target.toml") -> None:
        """Write one named board configuration with its target identity filled in."""
        template = (PROFILE_FIXTURES / fixture).read_text(encoding="utf-8")
        (self.root / "targets" / target / "target.toml").write_text(
            template % {"target": target},
            encoding="utf-8",
        )


@pytest.fixture
def profile_context(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ProfileContext:
    """Bind board loading to one owned profile checkout for this scenario."""
    context = ProfileContext(tmp_path)
    monkeypatch.setattr(common, "ROOT", context.root)
    monkeypatch.setattr(targets, "load_platform", mock.Mock(return_value=context.platform))
    return context


class GlobalProfileTests:
    """Keep target features common while changing the system root."""

    @staticmethod
    @pytest.mark.usefixtures("profile_context")
    def test_default_alias_has_the_same_identity_and_configuration() -> None:
        """Spelling default explicitly does not create a second build context."""
        target = "first"
        implicit = targets.load_target(target)
        explicit = targets.load_target(target, "default")
        assert (implicit) == (explicit)
        assert (explicit["profile"]) is None
        assert (explicit["linux"]["root"]) == ({"kind": "initramfs"})
        assert (explicit["bootstrap"]["kind"]) == ("linux")

    @staticmethod
    @pytest.mark.usefixtures("profile_context")
    def test_root_selection_keeps_feature_policy_and_board_stage0_outputs() -> None:
        """MicroSD changes root placement but keeps feature and firmware inputs."""
        target = "first"
        ram = targets.load_target(target)
        card = targets.load_target(target, "microsd-uboot")
        assert (paths.discover_profiles(target)) == (("default", "microsd-uboot"))
        assert (card["profile"]) == ("microsd-uboot")
        assert (card["linux"]["root"]) == (
            {
                "kind": "external",
                "filesystem": "ext4",
                "wait_seconds": 10,
                "partuuid": "46504c58-02",
            }
        )
        assert (ram["rootfs"]["packages"]) == (["fplinux-soundfont-lite"])
        assert "fplinux-soundfont" in card["rootfs"]["packages"]
        assert (card["rootfs"]["base_packages"]) == (ram["rootfs"]["base_packages"])
        assert (card["device_data"]["groups"]["bluetooth"]) == (
            [
                {
                    "source": "radio.bin",
                    "destination": "demo/radio.bin",
                    "size": 4,
                }
            ]
        )
        assert (card["device_data"]) == (ram["device_data"])
        assert (card["linux"]["memory"]) == ({"base": 0x80200000, "size": 0x03C00000})
        assert card["bootstrap"]["image"] == "sd-stage0.bin"
        assert card["bootstrap"]["map"] == "sd-stage0.map"
        assert (card["runtime"]) == (ram["runtime"])

    @staticmethod
    @pytest.mark.usefixtures("profile_context")
    @pytest.mark.parametrize("marker", ["spi-nand", "mtd", "ubi", "emmc", "forbidden"])
    def test_storage_policy_rejects_common_and_board_dtb_nodes(marker: str) -> None:
        """A board's additions cannot replace the mandatory internal-storage exclusions."""
        config = targets.load_target("first")
        with pytest.raises(SystemExit, match=f"forbidden storage marker {marker}"):
            verify_forbidden_dtb_markers(
                binary_tree([], [(marker.upper(), [], [])]),
                config["linux"]["forbidden_dtb_markers"],
            )

    @pytest.mark.parametrize("old_field", ["forbidden_config", "forbidden_dtb_markers"])
    def test_storage_policy_rejects_replaced_raw_fields(
        self, profile_context: ProfileContext, old_field: str
    ) -> None:
        """Manifests must declare board additions through the current explicit schema."""
        manifest = profile_context.root / "targets/first/target.toml"
        manifest.write_text(manifest.read_text().replace(f"additional_{old_field}", old_field))
        with pytest.raises(SystemExit, match="target linux"):
            targets.load_target("first")

    def test_board_local_directory_cannot_define_a_global_profile(
        self, profile_context: ProfileContext
    ) -> None:
        """A board-local profile directory cannot change the global selection."""
        board_profile = profile_context.root / "targets/first/profiles/bt-qual"
        board_profile.mkdir(parents=True)
        (board_profile / "profile.toml").write_text("invalid = true\n")
        assert (paths.discover_profiles("first")) == (("default", "microsd-uboot"))
        with pytest.raises(SystemExit, match="unknown profile"):
            targets.load_target("first", "bt-qual")
        assert (targets.load_target("first")["linux"]["root"]) == ({"kind": "initramfs"})

    def test_boot_profile_cannot_exclude_shared_features(
        self, profile_context: ProfileContext
    ) -> None:
        """Feature-package ownership is rejected at the boot-policy boundary."""
        path = profile_context.root / "profiles/default/profile.toml"
        shutil.copyfile(PROFILE_FIXTURES / "feature-package/profile.toml", path)
        with pytest.raises(SystemExit, match="system content policy"):
            targets.load_target("first")

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_missing_bluetooth_declaration_does_not_require_private_inputs(
        self, profile_context: ProfileContext, profile: str
    ) -> None:
        """A board without a Bluetooth declaration has no firmware input group."""
        profile_context.write_target_manifest("first", "without-bluetooth/target.toml")
        assert (targets.load_target("first", profile)["device_data"]["groups"]) == ({})

    @pytest.mark.parametrize(
        "display_settings",
        [
            pytest.param(
                'backlight_channels = "mono"\nbacklight_level = 0\n', id="missing-spi-mode"
            ),
            pytest.param("spi_mode = 0\nbacklight_level = 0\n", id="missing-backlight-channels"),
            pytest.param(
                'spi_mode = 0\nbacklight_channels = "mono"\n', id="missing-backlight-level"
            ),
            pytest.param("lcd_id = 0\n", id="direct-panel-id"),
        ],
    )
    def test_loader_adapter_rejects_partial_display_settings_and_a_direct_panel_id(
        self, profile_context: ProfileContext, display_settings: str
    ) -> None:
        """Display settings may be omitted; partial settings and an explicit lcd_id are invalid."""
        path = profile_context.root / "targets/first/target.toml"
        manifest = path.read_text(encoding="utf-8")
        assert (targets.load_target("first")["runtime"]["adapter"]) == (
            {
                "exec_distance": 0,
                "session_name": "first",
                "boot_instructions": "Power off and connect.",
            }
        )

        path.write_text(
            manifest.replace("[adapter]\n", "[adapter]\n" + display_settings), encoding="utf-8"
        )
        with (
            pytest.raises(SystemExit, match="target adapter must contain exactly"),
        ):
            targets.load_target("first")

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_fm_declaration_adds_only_the_declared_device_data_group(
        self, profile_context: ProfileContext, profile: str
    ) -> None:
        """An FM firmware declaration has the same group contract in both boot profiles."""
        path = profile_context.root / "targets/first/target.toml"
        path.write_text(
            path.read_text(encoding="utf-8") + '\n[fm_radio]\nfirmware = [{ source = "radio.bin", '
            'destination = "fplinux/radio.bin", size = 128 }]\n',
            encoding="utf-8",
        )
        groups = targets.load_target("first", profile)["device_data"]["groups"]
        assert (groups["fm-radio"]) == (
            [{"source": "radio.bin", "destination": "fplinux/radio.bin", "size": 128}]
        )
        assert (set(groups)) == ({"bluetooth", "fm-radio"})
        assert ("fm-radio") not in (targets.load_target("second")["device_data"]["groups"])

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_first_stage_loader_is_shared_across_boards_and_boot_profiles(
        self, profile_context: ProfileContext, profile: str
    ) -> None:
        """Platform loader inputs survive target selection and a different system root."""
        del profile_context
        config = targets.load_target("first", profile)
        assert config["runtime"]["assets"] == {"fdl1": "assets/loader.bin"}
        assert config["loader_assets"][0]["output"] == [
            {
                "role": "fdl1",
                "path": "loader.bin",
                "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            },
        ]
        assert targets.load_target("second", profile)["runtime"]["assets"] == {
            "fdl1": "assets/loader.bin"
        }

    @pytest.mark.parametrize(
        ("table", "group"), [("fm_radio", "fm-radio"), ("audio_profile", "audio-profile")]
    )
    def test_audio_and_fm_firmware_require_their_exact_size(
        self, profile_context: ProfileContext, table: str, group: str
    ) -> None:
        """Audio and FM inputs cannot omit the size that device-data consumers verify."""
        profile_context.write_target_manifest("first", "without-bluetooth/target.toml")
        path = profile_context.root / "targets/first/target.toml"
        base = path.read_text(encoding="utf-8")
        declaration = (
            '\n[device_data]\nparser = "radio_parser.py"\n'
            f"\n[{table}]\n"
            'firmware = [{ source = "radio.bin", destination = "demo/radio.bin", size = 4 }]\n'
        )
        path.write_text(base + declaration, encoding="utf-8")
        assert targets.load_target("first")["device_data"]["groups"] == {
            group: [{"source": "radio.bin", "destination": "demo/radio.bin", "size": 4}]
        }
        path.write_text(base + declaration.replace(", size = 4", ""), encoding="utf-8")
        with pytest.raises(SystemExit, match=r"source, destination, size and optional sha256"):
            targets.load_target("first")

    def test_bluetooth_firmware_can_leave_size_to_extraction(
        self, profile_context: ProfileContext
    ) -> None:
        """Bluetooth inputs preserve the supported optional-size declaration."""
        path = profile_context.root / "targets/first/target.toml"
        path.write_text(
            path.read_text(encoding="utf-8").replace("size = 4\n", ""), encoding="utf-8"
        )
        assert targets.load_target("first")["device_data"]["groups"] == {
            "bluetooth": [{"source": "radio.bin", "destination": "demo/radio.bin"}]
        }

    @pytest.mark.parametrize(
        ("source", "message"),
        [
            ("platforms/demo/uboot/board_defconfig", "U-Boot source is missing"),
            ("platforms/demo/bootstrap-microsd", "platform microSD bootstrap source is missing"),
        ],
    )
    def test_missing_platform_boot_sources_fail_only_when_microsd_is_selected(
        self, profile_context: ProfileContext, source: str, message: str
    ) -> None:
        """RAM operation does not read unused platform Stage0 and U-Boot sources."""
        path = profile_context.root / source
        if path.is_dir():
            path.rmdir()
        else:
            path.unlink()
        assert (targets.load_target("second")["bootstrap"]["kind"]) == ("linux")
        with pytest.raises(SystemExit, match=message):
            targets.load_target("second", "microsd-uboot")

    def test_board_without_microsd_inputs_offers_only_the_ram_profile(
        self, profile_context: ProfileContext
    ) -> None:
        """Omitting the microSD tables removes that profile instead of borrowing inputs."""
        profile_context.write_target_manifest("first", "without-microsd/target.toml")

        assert (paths.discover_profiles("first")) == (("default",))
        assert (targets.load_target("first")["bootstrap"]["kind"]) == ("linux")
        with pytest.raises(
            SystemExit, match="target first does not support profile microsd-uboot"
        ):
            targets.load_target("first", "microsd-uboot")
        assert (paths.discover_profiles("second")) == (("default", "microsd-uboot"))

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_declared_microsd_inputs_must_stay_complete(
        self, profile_context: ProfileContext, profile: str
    ) -> None:
        """A partial microSD declaration is still refused, even for the RAM profile."""
        path = profile_context.root / "targets/first/target.toml"
        manifest = path.read_text(encoding="utf-8")
        path.write_text(
            manifest.partition("[microsd.bootstrap]")[0] + "[microsd]\n", encoding="utf-8"
        )

        with (
            pytest.raises(SystemExit, match="target microsd must contain exactly"),
        ):
            targets.load_target("first", profile)

    @pytest.mark.parametrize(
        ("target", "stage0_image"),
        [
            ("bq-2830l-tank-connect-max", "bq_2830l_tank_connect_max_sd_stage0.bin"),
            ("inoi-240-modern-4g", "inoi240_sd_stage0.bin"),
            ("inoi-244-modern-4g", "inoi244_sd_stage0.bin"),
            ("itel-neo-r60-plus", "itel_neo_r60_plus_sd_stage0.bin"),
            ("maxvi-k15n-4g", "maxvi_k15n_4g_sd_stage0.bin"),
            ("nokia-110-4g", "nokia_110_4g_sd_stage0.bin"),
            ("nokia-ta1618", "ta1618_sd_stage0.bin"),
            ("uniwa-v1000-4g", "uniwa_v1000_sd_stage0.bin"),
        ],
    )
    def test_ums9117_system_card_preserves_board_boot_policy(
        self, target: str, stage0_image: str
    ) -> None:
        """All eight released UMS9117 targets offer the intended system-card entry points."""
        assert paths.discover_profiles(target) == ("default", "microsd-uboot")
        card = targets.load_target(target, "microsd-uboot")
        assert card["bootstrap"]["image"] == stage0_image
        assert card["linux"]["memory"] == targets.load_target(target)["linux"]["memory"]
        generated = generated_bootstrap_files(card, platforms.load_platform("ums9117"))
        assert "generated/fplinux-bootstrap-options.h" in generated

    def test_itel_system_card_stage0_is_headless(self) -> None:
        """The Linux panel remains available after the headless loader hands off."""
        card = targets.load_target("itel-neo-r60-plus", "microsd-uboot")
        assert card["display"]["width"] > 0
        assert card["display"]["height"] > 0
        assert bootstrap_display(card) == {"width": 0, "height": 0}
        generated = generated_bootstrap_files(card, platforms.load_platform("ums9117"))
        assert b'"--headless"' in generated["generated/fplinux-bootstrap-options.h"]

    def test_konrow_stays_on_ram_and_qin_keeps_its_developer_profile(self) -> None:
        """Extra UMS9117 cards do not enable Konrow or change Qin's Linux entry."""
        assert paths.discover_profiles("konrow-senior-280-plus") == ("default",)
        assert paths.discover_profiles("xiaomi-qin-1s-plus") == ("default", "microsd-root")
        qin = targets.load_target("xiaomi-qin-1s-plus", "microsd-root")
        assert qin["bootstrap"]["kind"] == "linux"
        assert qin["uboot"]["kind"] == "none"
        assert qin["linux"]["root"]["kind"] == "external"

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_nand_reader_stays_board_owned_across_boot_profiles(
        self, profile_context: ProfileContext, profile: str
    ) -> None:
        """Boot policy preserves an explicit reader and supplies none to other boards."""
        path = profile_context.root / "targets/first/target.toml"
        path.write_text(
            path.read_text()
            + '\n[nand]\nraw_device = "/dev/first-nand-raw"\nid = 0xb1a1\nraw_page_bytes = 2176\n'
        )
        assert (targets.load_target("first", profile)["nand"]) == (
            {"raw_device": "/dev/first-nand-raw", "id": 0xB1A1, "raw_page_bytes": 2176}
        )
        assert ("nand") not in (targets.load_target("second", profile))

    @pytest.mark.parametrize(
        ("table", "expected"),
        [
            pytest.param(
                'raw_device = "/dev/first-nand-raw"\n',
                {"raw_device": "/dev/first-nand-raw"},
                id="reader-only",
            ),
            pytest.param(
                'raw_device = "/dev/first-nand-raw"\nid = 0xc1c8\nraw_page_bytes = 2176\n',
                {"raw_device": "/dev/first-nand-raw", "id": 0xC1C8, "raw_page_bytes": 2176},
                id="unlisted-chip",
            ),
        ],
    )
    def test_nand_chip_declaration_is_optional_and_not_limited_to_known_chips(
        self, profile_context: ProfileContext, table: str, expected: dict[str, str | int]
    ) -> None:
        """A board can name only its reader, or expect a chip the host has not seen before."""
        path = profile_context.root / "targets/first/target.toml"
        base = path.read_text()
        path.write_text(base + "\n[nand]\n" + table)
        assert (targets.load_target("first")["nand"]) == (expected)

    @pytest.mark.parametrize(
        "declaration",
        [
            pytest.param("id = 0xb1a1\n", id="missing-page-size"),
            pytest.param("raw_page_bytes = 2176\n", id="missing-chip-id"),
        ],
    )
    def test_nand_chip_declaration_requires_both_id_and_page_size(
        self, profile_context: ProfileContext, declaration: str
    ) -> None:
        """Half a chip declaration cannot silently disable the device comparison."""
        path = profile_context.root / "targets/first/target.toml"
        base = path.read_text()
        path.write_text(base + '\n[nand]\nraw_device = "/dev/first-nand-raw"\n' + declaration)
        with (
            pytest.raises(SystemExit, match="requires both id and raw_page_bytes"),
        ):
            targets.load_target("first")

    def test_parallel_nand_requires_a_complete_target_declaration(
        self, profile_context: ProfileContext
    ) -> None:
        """A board selects parallel NAND with an explicit, self-consistent physical layout."""
        path = profile_context.root / "targets/first/target.toml"
        base = path.read_text()
        declaration = (
            '\n[nand]\nraw_device = "/dev/first-nand-raw"\ninterface = "parallel"\n'
            "id = 0xba98\nraw_page_bytes = 1088\npage_main_bytes = 1024\n"
            "oob_bytes = 64\npages_per_block = 64\n"
        )
        path.write_text(base + declaration)
        with pytest.raises(SystemExit, match="parallel interface requires a complete NAND layout"):
            targets.load_target("first")

        path.write_text(base + declaration + "block_count = 2048\n")
        nand = targets.load_target("first")["nand"]
        assert nand["interface"] == "parallel"
        assert nand["block_count"] == 2048

    def test_nand_reader_requires_a_device_path(self, profile_context: ProfileContext) -> None:
        """A board cannot accidentally point a physical NAND backup at a regular file."""
        path = profile_context.root / "targets/first/target.toml"
        path.write_text(
            path.read_text()
            + '\n[nand]\nraw_device = "/tmp/nand.raw"\nid = 0xb1a1\nraw_page_bytes = 2176\n'
        )
        with pytest.raises(SystemExit, match="device directly under /dev"):
            targets.load_target("first")


class KernelConfigCompositionTests:
    """The shared base and board fragment produce one unambiguous Kconfig input."""

    @staticmethod
    def test_build_type_policy_overrides_board_values_and_checks_resolved_capabilities(
        tmp_path: Path,
    ) -> None:
        """A lost enabled diagnostic or unexpectedly enabled release feature fails validation."""
        root = tmp_path
        base = root / "base"
        board = root / "board"
        policy = root / "release"
        actual = root / ".config"
        base.write_text("CONFIG_KALLSYMS=y\nCONFIG_MODULES=y\n")
        board.write_text("CONFIG_DEVMEM=y\nCONFIG_LOG_BUF_SHIFT=17\n")
        policy.write_text(
            "# CONFIG_MODULES is not set\n# CONFIG_DEVMEM is not set\nCONFIG_LOG_BUF_SHIFT=16\n"
        )
        composed = kernel.compose_kernel_config(base, board, policy, boot_font="6x8")
        assert (composed) == (
            b"CONFIG_KALLSYMS=y\n# CONFIG_MODULES is not set\n"
            b"# CONFIG_DEVMEM is not set\nCONFIG_LOG_BUF_SHIFT=16\n"
            b"CONFIG_FONTS=y\nCONFIG_FONT_6x8=y\n"
        )
        actual.write_bytes(composed)
        kernel_build.assert_build_type_kconfig(actual, policy, "release")
        actual.write_text("CONFIG_MODULES=y\nCONFIG_LOG_BUF_SHIFT=16\n")
        with pytest.raises(SystemExit, match="release build did not preserve CONFIG_MODULES=n"):
            kernel_build.assert_build_type_kconfig(actual, policy, "release")
        policy.write_text("CONFIG_MODULES=y\nCONFIG_DEVMEM=y\nCONFIG_LOG_BUF_SHIFT=17\n")
        with pytest.raises(SystemExit, match="debug build did not preserve CONFIG_DEVMEM=y"):
            kernel_build.assert_build_type_kconfig(actual, policy, "debug")

    @staticmethod
    def test_board_values_override_the_base_without_losing_shared_features(tmp_path: Path) -> None:
        """Both enabled and explicitly disabled board settings replace base values."""
        base = tmp_path / "base"
        fragment = tmp_path / "fragment"
        base.write_text("CONFIG_SHARED=y\nCONFIG_BOARD=1\nCONFIG_UNUSED=y\n")
        fragment.write_text("CONFIG_BOARD=2\n# CONFIG_UNUSED is not set\nCONFIG_DEVICE=y\n")
        assert (kernel.compose_kernel_config(base, fragment, boot_font="7x14")) == (
            b"CONFIG_SHARED=y\nCONFIG_BOARD=2\n# CONFIG_UNUSED is not set\nCONFIG_DEVICE=y\n"
            b"CONFIG_FONTS=y\nCONFIG_FONT_7x14=y\n"
        )


class BuildTypeSelectionTests:
    """Build type selection preserves boot policy, optional APKs and prepared sources."""

    @staticmethod
    @pytest.mark.parametrize(("target", "profile"), REPOSITORY_PROFILES)
    def test_debug_adds_tools_without_changing_boot_bundle_or_linux_source_recipe(
        target: str, profile: str
    ) -> None:
        """Debug adds tracing tools while retaining boot policy, optional APKs and sources."""
        sources = common.load_toml(ROOT / "sources.lock.toml")
        release = targets.load_target(target, profile)
        debug = targets.load_target(target, profile, build_type="debug")
        platform = platforms.load_platform(release["platform"])
        assert (release["build_type"]) == ("release")
        assert (debug["build_type"]) == ("debug")
        assert (release["linux"]["root"]) == (debug["linux"]["root"])
        assert (release["profile"]) == (debug["profile"])
        release_packages = alpine_state.selected_packages(platform, release)
        debug_packages = alpine_state.selected_packages(platform, debug)
        assert "fplinux-debug-tools" not in release_packages
        assert debug_packages == tuple(sorted((*release_packages, "fplinux-debug-tools")))
        assert (alpine_state.bundle_packages(platform, release, release_packages)) == (
            alpine_state.bundle_packages(platform, debug, debug_packages)
        )
        linux = sources[platform["linux"]["source_lock"]]
        assert (linux_build.linux_recipe_digest(linux, target, release, platform)) == (
            linux_build.linux_recipe_digest(linux, target, debug, platform)
        )

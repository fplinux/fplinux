# SPDX-License-Identifier: GPL-2.0-only
"""Global boot-policy selection and board configuration composition."""

from __future__ import annotations

import shutil
from typing import TYPE_CHECKING, Any
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.alpine import selection as alpine_state
from fplinux_cli.build.kernel import configuration as kernel_build
from fplinux_cli.build.kernel import prepare as linux_build
from fplinux_cli.common import ROOT
from fplinux_cli.manifests import kernel, paths, platforms, targets

if TYPE_CHECKING:
    from pathlib import Path

PROFILE_FIXTURES = ROOT / "tests/fixtures/profile_config"
REPOSITORY_PROFILES = [
    pytest.param(target, profile, id=f"{target}-{profile}")
    for target in paths.discover_targets()
    for profile in paths.discover_profiles(target)
]


class GlobalProfileTests:
    """Keep target features common while changing the system root."""

    @pytest.fixture(autouse=True)
    def _profile_repository(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
        """Provide two board manifests and real global boot-policy inputs."""
        self.root = tmp_path
        shutil.copytree(ROOT / "profiles", self.root / "profiles")
        self.platform: dict[str, Any] = {
            "identity": {"compatible": "demo,soc"},
            "rootfs": {"packages": ["fplinux-ssh", "fplinux-feature"]},
            "linux": {"defconfig": "platforms/demo/kernel/defconfig"},
            "uboot": {
                "source": "platforms/demo/uboot.lock.toml",
                "archive_prefix": "u-boot-2026.07",
                "patches": [],
                "required_config": ["CONFIG_TARGET_DEMO=y"],
                "copies": [],
            },
            "bootstrap": {
                "kernel_destination": "zImage",
                "load_address": 0x80100000,
                "payload_limit": 0x82000000,
                "layout": {
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
                },
                "toolchain": "toolchain",
                "lto": 0,
            },
            "runtime": {"fdl1_load_address": 0, "adapter": {}, "usb": {}},
        }
        shared = self.root / "platforms/demo"
        (shared / "kernel").mkdir(parents=True)
        (shared / "kernel/defconfig").write_text("CONFIG_FEATURE=y\nCONFIG_BOARD=1\n")
        (shared / "uboot.lock.toml").write_text(
            'version = "2026.07"\nrepository = "https://example.invalid/u-boot"\n'
            'tag = "v2026.07"\ncommit = "' + "a" * 40 + '"\n'
            'archive_url = "https://example.invalid/u-boot.tar.bz2"\n'
            'archive_sha256 = "' + "b" * 64 + '"\nlicense = "GPL-2.0-only"\n'
        )
        for target in ("first", "second"):
            target_root = self.root / "targets" / target
            (target_root / "kernel").mkdir(parents=True)
            (target_root / "kernel/config.fragment").write_text("CONFIG_BOARD=2\n")
            (target_root / "bootstrap-microsd").mkdir()
            (target_root / "uboot").mkdir()
            (target_root / "uboot/defconfig").write_text("CONFIG_ARM=y\n")
            self.write_target_manifest(target)
        monkeypatch.setattr(common, "ROOT", self.root)
        monkeypatch.setattr(targets, "load_platform", mock.Mock(return_value=self.platform))
        monkeypatch.setattr(targets, "asset_bundle_paths", mock.Mock(return_value={}))

    def write_target_manifest(self, target: str, fixture: str = "target.toml") -> None:
        """Write one named board configuration with its target identity filled in."""
        template = (PROFILE_FIXTURES / fixture).read_text(encoding="utf-8")
        (self.root / "targets" / target / "target.toml").write_text(
            template % {"target": target},
            encoding="utf-8",
        )

    @staticmethod
    @pytest.mark.parametrize("target", ["first", "second"])
    def test_default_alias_has_the_same_identity_and_configuration(target: str) -> None:
        """Spelling default explicitly does not create a second build context."""
        implicit = targets.load_target(target)
        explicit = targets.load_target(target, "default")
        assert (implicit) == (explicit)
        assert (explicit["profile"]) is None
        assert (explicit["linux"]["root"]) == ({"kind": "initramfs"})
        assert (explicit["bootstrap"]["kind"]) == ("linux")

    @staticmethod
    @pytest.mark.parametrize("target", ["first", "second"])
    def test_both_boards_share_boot_policy_and_keep_their_own_boot_sources(target: str) -> None:
        """MicroSD changes root placement but keeps feature and firmware inputs."""
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
        assert (ram["rootfs"]["packages"]) == ([])
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
        assert (card["linux"]["config_fragment"]) == (ram["linux"]["config_fragment"])
        assert (card["bootstrap"]["source"]) == ("bootstrap-microsd")
        assert (card["uboot"]["defconfig"]) == ("uboot/defconfig")
        assert (card["runtime"]) == (ram["runtime"])

    def test_board_local_directory_cannot_define_a_global_profile(self) -> None:
        """A board-local profile directory cannot change the global selection."""
        board_profile = self.root / "targets/first/profiles/bt-qual"
        board_profile.mkdir(parents=True)
        (board_profile / "profile.toml").write_text("invalid = true\n")
        assert (paths.discover_profiles("first")) == (("default", "microsd-uboot"))
        with pytest.raises(SystemExit, match="unknown profile"):
            targets.load_target("first", "bt-qual")
        assert (targets.load_target("first")["linux"]["root"]) == ({"kind": "initramfs"})

    def test_boot_profile_cannot_exclude_shared_features(self) -> None:
        """Feature-package ownership is rejected at the boot-policy boundary."""
        path = self.root / "profiles/default/profile.toml"
        shutil.copyfile(PROFILE_FIXTURES / "default-with-feature-package.toml", path)
        with pytest.raises(SystemExit, match="boot maintenance"):
            targets.load_target("first")

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_missing_bluetooth_declaration_does_not_require_private_inputs(
        self, profile: str
    ) -> None:
        """A board without a Bluetooth declaration has no firmware input group."""
        self.write_target_manifest("first", "target-without-bluetooth.toml")
        assert (targets.load_target("first", profile)["device_data"]["groups"]) == ({})

    @pytest.mark.parametrize(
        "missing_setting",
        [
            pytest.param("spi_mode = 0\n", id="spi-mode"),
            pytest.param("lcd_id = 0\n", id="lcd-id"),
            pytest.param('backlight_channels = "mono"\n', id="backlight-channels"),
            pytest.param("backlight_level = 0\n", id="backlight-level"),
        ],
    )
    def test_loader_display_settings_are_declared_together_or_omitted(
        self, missing_setting: str
    ) -> None:
        """A board without the LCD settings loads headless; a partial set is refused."""
        path = self.root / "targets/first/target.toml"
        manifest = path.read_text(encoding="utf-8")
        display_lines = (
            "spi_mode = 0\n",
            "lcd_id = 0\n",
            'backlight_channels = "mono"\n',
            "backlight_level = 0\n",
        )
        headless = manifest
        for line in display_lines:
            headless = headless.replace(line, "")
        path.write_text(headless, encoding="utf-8")
        assert (targets.load_target("first")["runtime"]["adapter"]) == (
            {
                "exec_distance": 0,
                "session_name": "first",
                "boot_instructions": "Power off and connect.",
            }
        )

        path.write_text(manifest.replace(missing_setting, ""), encoding="utf-8")
        with (
            pytest.raises(SystemExit, match="target adapter must contain exactly"),
        ):
            targets.load_target("first")

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_fm_declaration_adds_only_the_declared_device_data_group(self, profile: str) -> None:
        """An FM firmware declaration has the same group contract in both boot profiles."""
        path = self.root / "targets/first/target.toml"
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

    def test_board_maps_declaration_alone_needs_no_target_parser_or_sizes(self) -> None:
        """The target loader accepts board-map declarations without a target parser or size."""
        self.write_target_manifest("first", "target-without-bluetooth.toml")
        path = self.root / "targets/first/target.toml"
        declaration = (
            '\n[board_maps]\nfirmware = [{ source = "pinmap.bin", destination = "pinmap.bin" }]\n'
        )
        path.write_text(path.read_text(encoding="utf-8") + declaration, encoding="utf-8")
        assert (targets.load_target("first")["device_data"]) == (
            {"groups": {"board-maps": [{"source": "pinmap.bin", "destination": "pinmap.bin"}]}}
        )

    def test_parser_firmware_must_declare_its_exact_size(self) -> None:
        """Bluetooth, FM and audio inputs keep the exact size that builds verify."""
        self.write_target_manifest("first")
        path = self.root / "targets/first/target.toml"
        sized = 'destination = "demo/radio.bin"\nsize = 4\n'
        text = path.read_text(encoding="utf-8")
        assert (sized) in (text)
        path.write_text(text.replace(sized, 'destination = "demo/radio.bin"\n'), encoding="utf-8")
        with pytest.raises(SystemExit, match=r"source, destination, size and optional sha256"):
            targets.load_target("first")

    def test_missing_board_boot_sources_fail_only_when_microsd_is_selected(self) -> None:
        """RAM operation does not read unused U-Boot sources or fall back to another board."""
        (self.root / "targets/second/uboot/defconfig").unlink()
        assert (targets.load_target("second")["bootstrap"]["kind"]) == ("linux")
        with pytest.raises(SystemExit, match="U-Boot source is missing"):
            targets.load_target("second", "microsd-uboot")

    def test_board_without_microsd_inputs_offers_only_the_ram_profile(self) -> None:
        """Omitting the microSD tables removes that profile instead of borrowing inputs."""
        self.write_target_manifest("first", "target-without-microsd.toml")

        assert (paths.discover_profiles("first")) == (("default",))
        assert (targets.load_target("first")["bootstrap"]["kind"]) == ("linux")
        with pytest.raises(
            SystemExit, match="target first does not support profile microsd-uboot"
        ):
            targets.load_target("first", "microsd-uboot")
        assert (paths.discover_profiles("second")) == (("default", "microsd-uboot"))

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_declared_microsd_inputs_must_stay_complete(self, profile: str) -> None:
        """A partial microSD declaration is still refused, even for the RAM profile."""
        path = self.root / "targets/first/target.toml"
        manifest = path.read_text(encoding="utf-8")
        uboot = '[microsd.uboot]\ndefconfig = "uboot/defconfig"\npatches = []\ncopies = []\n'
        assert (uboot) in (manifest)
        path.write_text(manifest.replace(uboot, ""), encoding="utf-8")

        with (
            pytest.raises(SystemExit, match="target microsd must contain exactly"),
        ):
            targets.load_target("first", profile)

    @pytest.mark.parametrize("target", ["first", "second"])
    def test_uboot_combines_shared_sources_with_the_selected_board(self, target: str) -> None:
        """The target loader combines shared and selected board inputs in the U-Boot config."""
        shared = self.root / "platforms/demo"
        (shared / "boot.c").write_text("shared boot flow\n")
        (shared / "boot.patch").write_text("shared integration patch\n")
        self.platform["uboot"]["patches"] = ["platforms/demo/boot.patch"]
        self.platform["uboot"]["required_config"] = [
            "CONFIG_TARGET_DEMO=y",
            'CONFIG_BOOTCOMMAND="demoboot"',
        ]
        self.platform["uboot"]["copies"] = [
            {"source": "platforms/demo/boot.c", "destination": "board/demo/boot.c"}
        ]
        target_root = self.root / "targets" / target
        (target_root / "uboot/slot.c").write_text(f"{target} slot\n")
        (target_root / "uboot/board.patch").write_text(f"{target} integration patch\n")
        self.write_target_manifest(target, "target-with-uboot-sources.toml")

        uboot = targets.load_target(target, "microsd-uboot")["uboot"]
        assert ([(self.root / path).read_text() for path in uboot["patches"]]) == (
            ["shared integration patch\n", f"{target} integration patch\n"]
        )
        projected = {
            step["destination"]: (self.root / step["source"]).read_text()
            for step in uboot["copies"]
        }
        assert (projected) == (
            {
                "board/demo/boot.c": "shared boot flow\n",
                "board/demo/slot.c": f"{target} slot\n",
            }
        )
        assert (uboot["required_config"]) == (
            ["CONFIG_TARGET_DEMO=y", 'CONFIG_BOOTCOMMAND="demoboot"']
        )

    @pytest.mark.parametrize("profile", ["default", "microsd-uboot"])
    def test_nand_reader_stays_board_owned_across_boot_profiles(self, profile: str) -> None:
        """Boot policy preserves an explicit reader and supplies none to other boards."""
        path = self.root / "targets/first/target.toml"
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
        self, table: str, expected: dict[str, str | int]
    ) -> None:
        """A board can name only its reader, or expect a chip the host has not seen before."""
        path = self.root / "targets/first/target.toml"
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
    def test_nand_chip_declaration_requires_both_id_and_page_size(self, declaration: str) -> None:
        """Half a chip declaration cannot silently disable the device comparison."""
        path = self.root / "targets/first/target.toml"
        base = path.read_text()
        path.write_text(base + '\n[nand]\nraw_device = "/dev/first-nand-raw"\n' + declaration)
        with (
            pytest.raises(SystemExit, match="optionally both id and raw_page_bytes"),
        ):
            targets.load_target("first")

    def test_nand_reader_requires_a_device_path(self) -> None:
        """A board cannot accidentally point a physical NAND backup at a regular file."""
        path = self.root / "targets/first/target.toml"
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
        composed = kernel.compose_kernel_config(base, board, policy)
        assert (composed) == (
            b"CONFIG_KALLSYMS=y\n# CONFIG_MODULES is not set\n"
            b"# CONFIG_DEVMEM is not set\nCONFIG_LOG_BUF_SHIFT=16\n"
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
        assert (kernel.compose_kernel_config(base, fragment)) == (
            b"CONFIG_SHARED=y\nCONFIG_BOARD=2\n# CONFIG_UNUSED is not set\nCONFIG_DEVICE=y\n"
        )


class BuildTypeSelectionTests:
    """Kernel type selection preserves current rootfs and prepared-source consumers."""

    @staticmethod
    @pytest.mark.parametrize(("target", "profile"), REPOSITORY_PROFILES)
    def test_build_type_does_not_change_packages_root_or_linux_source_recipe(
        target: str, profile: str
    ) -> None:
        """Both types reuse each boot profile's unchanged packages and source projection."""
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
        assert (release_packages) == (debug_packages)
        assert (alpine_state.bundle_packages(platform, release, release_packages)) == (
            alpine_state.bundle_packages(platform, debug, debug_packages)
        )
        linux = sources[platform["linux"]["source_lock"]]
        assert (linux_build.linux_recipe_digest(linux, target, release, platform)) == (
            linux_build.linux_recipe_digest(linux, target, debug, platform)
        )


class RepositoryProfileCompressionTests:
    """Admit prepared Kconfig files only for the selected compressor policy."""

    @staticmethod
    @pytest.mark.parametrize(("target", "profile"), REPOSITORY_PROFILES)
    def test_each_profile_rejects_the_opposite_prepared_compressor(
        tmp_path: Path, target: str, profile: str
    ) -> None:
        """RAM admits ZSTD and microSD admits LZO-RLE for every target offering them."""
        prepared_configs = {
            "default": {
                "correct": (
                    "CONFIG_ZRAM=y\n"
                    "# CONFIG_ZRAM_BACKEND_LZO is not set\n"
                    "CONFIG_ZRAM_BACKEND_ZSTD=y\n"
                    "CONFIG_ZRAM_DEF_COMP_ZSTD=y\n"
                ),
                "opposite": (
                    "CONFIG_ZRAM=y\n"
                    "CONFIG_ZRAM_BACKEND_LZO=y\n"
                    "# CONFIG_ZRAM_BACKEND_ZSTD is not set\n"
                    "CONFIG_ZRAM_DEF_COMP_LZORLE=y\n"
                ),
            },
            "microsd-uboot": {
                "correct": (
                    "# CONFIG_BLK_DEV_INITRD is not set\n"
                    "CONFIG_EXT4_FS=y\n"
                    "CONFIG_ZRAM=y\n"
                    "CONFIG_ZRAM_BACKEND_LZO=y\n"
                    "# CONFIG_ZRAM_BACKEND_ZSTD is not set\n"
                    "CONFIG_ZRAM_DEF_COMP_LZORLE=y\n"
                ),
                "opposite": (
                    "# CONFIG_BLK_DEV_INITRD is not set\n"
                    "CONFIG_EXT4_FS=y\n"
                    "CONFIG_ZRAM=y\n"
                    "# CONFIG_ZRAM_BACKEND_LZO is not set\n"
                    "CONFIG_ZRAM_BACKEND_ZSTD=y\n"
                    "CONFIG_ZRAM_DEF_COMP_ZSTD=y\n"
                ),
            },
        }

        prepared = tmp_path / ".config"
        contents = prepared_configs[profile]
        linux = targets.load_target(target, profile)["linux"]
        compressor_enable = [
            symbol
            for symbol in linux["config_enable"]
            if symbol.startswith(("CONFIG_ZRAM_BACKEND_", "CONFIG_ZRAM_DEF_COMP_"))
        ]
        compressor_disable = [
            symbol
            for symbol in linux["config_disable"]
            if symbol.startswith(("CONFIG_ZRAM_BACKEND_", "CONFIG_ZRAM_DEF_COMP_"))
        ]
        prepared.write_text(contents["correct"])
        kernel_build.assert_profile_kconfig(
            prepared,
            compressor_enable,
            compressor_disable,
        )

        prepared.write_text(contents["opposite"])
        with pytest.raises(SystemExit, match=r"profile did not (enable|disable)"):
            kernel_build.assert_profile_kconfig(
                prepared,
                compressor_enable,
                compressor_disable,
            )

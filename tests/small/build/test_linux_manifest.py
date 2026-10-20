# SPDX-License-Identifier: GPL-2.0-only
"""Tests of Linux-only manifest discovery across source and platform boundaries."""

from __future__ import annotations

import tempfile
from pathlib import Path

import pytest
from fplinux_cli.manifests.linux import discover_linux_targets

from tests import ROOT
from tests.fixtures import linux_inputs


class LinuxManifestTests:
    """Collect compatible source integrations without loading peer build artifacts."""

    @staticmethod
    @pytest.mark.parametrize(
        ("declaration", "speaker_vibration"),
        [("", False), ("speaker_vibration = true\n", True)],
    )
    def test_peer_audio_profile_defaults_omitted_topology_flags_to_false(
        tmp_path: Path, declaration: str, *, speaker_vibration: bool
    ) -> None:
        """Peer loading preserves false defaults and an explicitly declared vibration policy."""
        linux_inputs.platform(tmp_path, "demo", source_lock="base", arch="arm")
        target = linux_inputs.target(tmp_path, "phone", platform_name="demo")
        with target.open("a", encoding="utf-8") as output:
            output.write("\n[audio_profile]\nfirmware = []\n" + declaration)

        targets = discover_linux_targets(tmp_path, {"base": {"sha256": "1" * 64}}, "1" * 64)

        assert targets[0].config["audio_policy"] == {
            "headfree_headphones_only": False,
            "speaker_from_headphones": False,
            "speaker_vibration": speaker_vibration,
        }

    @staticmethod
    def test_archive_digest_groups_distinct_platforms_and_architectures() -> None:
        """Lock aliases and differing ARCH values share only their exact archive digest."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            linux_inputs.platform(root, "first", source_lock="base", arch="arm")
            linux_inputs.platform(root, "second", source_lock="alias", arch="riscv")
            linux_inputs.platform(root, "third", source_lock="different", arch="arm")
            linux_inputs.target(root, "b-phone", platform_name="second", arch="riscv")
            linux_inputs.target(root, "a-phone", platform_name="first")
            linux_inputs.target(root, "c-phone", platform_name="third")
            sources = {
                "base": {"sha256": "1" * 64},
                "alias": {"sha256": "1" * 64},
                "different": {"sha256": "2" * 64},
            }

            targets = discover_linux_targets(root, sources, "1" * 64)

        assert ([target.name for target in targets]) == (["a-phone", "b-phone"])
        assert ([target.config["platform"] for target in targets]) == (["first", "second"])
        assert ([target.platform["linux"]["arch"] for target in targets]) == (["arm", "riscv"])
        assert ([target.platform["linux"]["dts_directory"] for target in targets]) == (
            ["arch/arm/boot/dts", "arch/riscv/boot/dts"]
        )
        assert (targets[0].config["identity"]["display_name"]) == ("Example a-phone")

    @staticmethod
    def test_linux_projection_ignores_peer_bootstrap_and_asset_requirements() -> None:
        """A peer's unbuildable loader or userspace does not hide its declared Linux inputs."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            platform = linux_inputs.platform(root, "demo", source_lock="base", arch="arm")
            target = linux_inputs.target(root, "phone", platform_name="demo")
            with platform.open("a", encoding="utf-8") as output:
                output.write(
                    '\n[bootstrap]\nsource = "absent-bootstrap"\n[rootfs]\npackages = 5\n'
                )
            with target.open("a", encoding="utf-8") as output:
                output.write("\n[adapter]\nboot_instructions = false\n[bluetooth]\nfirmware = 5\n")

            targets = discover_linux_targets(root, {"base": {"sha256": "1" * 64}}, "1" * 64)

        assert (len(targets)) == (1)
        assert {step["destination"] for step in targets[0].config["linux"]["copies"]} == {
            "arch/arm/boot/dts/phone.dts",
            "drivers/demo/phone.c",
        }

    @staticmethod
    def test_profile_patch_inputs_are_retained_for_projection_conflict_checks() -> None:
        """Source preparation can observe microSD patches even while building the RAM profile."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            linux_inputs.platform(root, "demo", source_lock="base", arch="arm")
            target = linux_inputs.target(root, "phone", platform_name="demo")
            target.write_text(
                target.read_text(encoding="utf-8").replace(
                    "linux_patches = []", 'linux_patches = ["linux/card.patch"]'
                ),
                encoding="utf-8",
            )

            targets = discover_linux_targets(root, {"base": {"sha256": "1" * 64}}, "1" * 64)

        assert (targets[0].config["microsd"]["linux_patches"]) == (["linux/card.patch"])

    @staticmethod
    def test_captured_overlay_and_panel_discovery_uses_only_snapshot_inputs(
        tmp_path: Path,
    ) -> None:
        """Later checkout changes cannot alter captured operations or generated panel inputs."""
        linux_inputs.platform(tmp_path, "demo", source_lock="base", arch="arm")
        manifest = linux_inputs.target(tmp_path, "phone", platform_name="demo")
        with manifest.open("a", encoding="utf-8") as output:
            output.write('\n[display]\npanel = "display/panel.toml"\nboot_font = "6x8"\n')
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                "[linux]\n",
                '[linux]\ndt_config_checks = [{path="/amplifier", '
                'compatible="example,amplifier", config="CONFIG_AMPLIFIER"}]\n',
            ),
            encoding="utf-8",
        )
        panel = tmp_path / "targets/phone/display/panel.toml"
        panel.parent.mkdir()
        panel.write_bytes((ROOT / "tests/fixtures/display/panel.toml").read_bytes())
        overlay = tmp_path / "targets/phone/linux/overlay"
        driver = linux_inputs.write(tmp_path, "targets/phone/linux/overlay/drivers/new.c", "old\n")
        captured = {
            path.relative_to(tmp_path).as_posix(): path.read_bytes()
            for path in tmp_path.rglob("*")
            if path.is_file()
        }
        driver.unlink()
        linux_inputs.write(tmp_path, "targets/phone/linux/overlay/drivers/later.c", "later\n")
        manifest.write_text("not TOML\n")
        panel.write_text("not TOML\n")

        discovered = discover_linux_targets(
            tmp_path, {"base": {"sha256": "1" * 64}}, "1" * 64, captured=captured
        )

        assert discovered[0].config["display"]["width"] == 128
        assert discovered[0].config["display"]["height"] == 160
        assert discovered[0].config["linux"]["dt_config_checks"] == [
            {
                "path": "/amplifier",
                "compatible": "example,amplifier",
                "config": "CONFIG_AMPLIFIER",
            }
        ]
        assert discovered[0].config["linux"]["copies"] == [
            {"source": "linux/board.dts", "destination": "arch/arm/boot/dts/phone.dts"},
            {
                "source": "linux/overlay/drivers/demo/phone.c",
                "destination": "drivers/demo/phone.c",
            },
            {"source": "linux/overlay/drivers/new.c", "destination": "drivers/new.c"},
        ]
        assert (overlay / "drivers/later.c").is_file()

# SPDX-License-Identifier: GPL-2.0-only
"""Exercise normalized kernel build plans with Kbuild and DT validation stand-ins."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import pytest
from fplinux_cli.alpine import rootfs_state
from fplinux_cli.build import process as process_build
from fplinux_cli.build.kernel import compile as kernel_build
from fplinux_cli.build.kernel import state as linux_state
from fplinux_cli.build.kernel.state import PreparedLinuxState
from fplinux_cli.environment.definition import environment_artifact_recipe_digest
from fplinux_cli.manifests.kernel import kconfig_values
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

_BOARD_STORAGE_EXCLUSIONS = (
    "CONFIG_MMC_REMOVABLE=y",
    "CONFIG_MMC_GPIO=y",
    "CONFIG_GPIO_EIC_SPRD=y",
    "CONFIG_GPIO_EIC_SPRD=m",
)


@dataclass
class KernelCommandPeer:
    """Replace the external compiler while retaining actual config and output receipts."""

    source: Path
    output: Path
    image: str
    dtb: str
    commands: list[list[str]] = field(default_factory=list)
    forbidden_config: str | None = None

    def run(self, command: list[str], **_options: object) -> None:
        """Apply scripts/config strings and provide deterministic compiler output files."""
        self.commands.append(command)
        if command[0] == str(self.source / "scripts/config"):
            actions = iter(command[3:])
            config = self.output / ".config"
            lines = config.read_text().splitlines()
            for action in actions:
                symbol = "CONFIG_" + next(actions)
                if action == "--set-str":
                    value = '"' + next(actions) + '"'
                elif action == "--enable":
                    value = "y"
                else:
                    assert action == "--disable"
                    value = "n"
                lines = [
                    line
                    for line in lines
                    if not line.startswith((symbol + "=", "# " + symbol + " "))
                ]
                lines.append(f"{symbol}={value}")
            config.write_text("\n".join(lines) + "\n")
        elif "olddefconfig" not in command:
            if self.forbidden_config is not None:
                with (self.output / ".config").open("a") as stream:
                    stream.write(self.forbidden_config + "\n")
            version = kconfig_values((self.output / ".config").read_text())["CONFIG_LOCALVERSION"]
            for relative, content in (
                (self.image, b"kernel " + version.encode()),
                (self.dtb, b"device tree"),
                ("vmlinux", b"kernel symbols"),
                ("System.map", b"kernel map"),
            ):
                path = self.output / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)


@pytest.mark.parametrize(
    ("boot_font", "font_symbol"),
    [("6x8", "CONFIG_FONT_6x8"), ("7x14", "CONFIG_FONT_7x14")],
)
@pytest.mark.parametrize(
    ("target", "delivery", "make_target", "additional_forbidden"),
    [
        pytest.param("nokia-110-4g", "embedded", "zImage", (), id="ums-embedded"),
        pytest.param("xiaomi-qin-1s-plus", "external", "Image", (), id="qin-external-ram"),
        pytest.param(
            "inoi-240-modern-4g",
            "embedded",
            "zImage",
            _BOARD_STORAGE_EXCLUSIONS,
            id="inoi-240-storage-guards",
        ),
        pytest.param(
            "inoi-244-modern-4g",
            "embedded",
            "zImage",
            _BOARD_STORAGE_EXCLUSIONS,
            id="inoi-244-storage-guards",
        ),
        pytest.param(
            "maxvi-k15n-4g",
            "embedded",
            "zImage",
            _BOARD_STORAGE_EXCLUSIONS,
            id="maxvi-storage-guards",
        ),
    ],
)
# Keep target expectations and owned test resources explicit.
def test_ram_build_uses_declared_image_delivery_and_causal_root_identity(  # noqa: PLR0913
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    target: str,
    delivery: str,
    make_target: str,
    additional_forbidden: tuple[str, ...],
    boot_font: str,
    font_symbol: str,
) -> None:
    """The actual target plan preserves RAM identity while selecting its declared image format."""
    target_config = load_target(target)
    target_config["display"]["boot_font"] = boot_font
    platform = load_platform(target_config["platform"])
    assert target_config["linux"]["root"] == {"kind": "initramfs"}
    assert target_config["bootstrap"]["initramfs_delivery"] == delivery

    source = tmp_path / "linux"
    (source / "scripts").mkdir(parents=True)
    (source / "scripts/config").write_text("external Kbuild command peer\n")
    output = tmp_path / "work/kernel"
    rootfs_output = tmp_path / "rootfs"
    rootfs_output.mkdir()
    rootfs = rootfs_output / "rootfs.cpio"
    rootfs.write_bytes(b"composition")
    initramfs = rootfs_output / "initramfs.cpio"
    initramfs.write_bytes(b"boot archive a")
    prepared = PreparedLinuxState("a" * 64, "b" * 64)
    peer = KernelCommandPeer(
        source,
        output,
        platform["linux"]["image_output"],
        str(Path(platform["linux"]["dtb_output_directory"]) / target_config["linux"]["dtb"]),
    )
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE", "c" * 64)
    monkeypatch.setattr(process_build, "run", peer.run)
    monkeypatch.setattr(linux_state, "require_prepared_linux", lambda *_args: prepared)
    monkeypatch.setattr(
        rootfs_state,
        "trusted_receipt_identity",
        lambda *_args: {"recipe": "d" * 64, "sha256": "e" * 64},
    )
    validators: list[str] = [
        "verify_dtb_kconfig",
        "verify_target_identity",
        "verify_root_bootargs",
        "verify_profile_dtb_layout",
        "assert_external_firmware_kconfig",
        "verify_builtin_audio_profile",
        "verify_builtin_panel_firmware",
    ]

    def ignore_validator(*_args: object) -> None:
        pass

    for name in validators:
        monkeypatch.setattr(kernel_build, name, ignore_validator)
    monkeypatch.setattr(kernel_build, "panel_firmware_kconfig_arguments", lambda *_args: [])

    def build() -> tuple[Path, Path, dict[str, str], str]:
        return kernel_build.build_kernel(
            target,
            target_config,
            platform,
            bootstrap_recipe="f" * 64,
            linux_source=source,
            prepared_linux=prepared,
            linux_base="1" * 64,
            output=output,
            cross=platform["linux"]["cross_compile"],
            rootfs=rootfs,
            rootfs_output=rootfs_output,
            rootfs_recipe="d" * 64,
            audio_profile_firmware=None,
            jobs=2,
        )

    image, _dtb, receipt, identity = build()
    assert image == output / "arch/arm/boot" / make_target
    assert peer.commands[-1][-2:] == [make_target, "dtbs"]
    config = kconfig_values((output / ".config").read_text())
    assert config["CONFIG_FONTS"] == "y"
    assert {
        symbol
        for symbol, value in config.items()
        if symbol.startswith("CONFIG_FONT_") and value == "y"
    } == {font_symbol}
    if delivery == "embedded":
        assert config["CONFIG_INITRAMFS_SOURCE"] == f'"{tmp_path}/work/rootfs.cpio"'
        assert (tmp_path / "work/rootfs.cpio").read_bytes() == b"boot archive a"
    else:
        assert config["CONFIG_INITRAMFS_SOURCE"] == '""'
        assert not (tmp_path / "work/rootfs.cpio").exists()

    command_count = len(peer.commands)
    assert build()[2:] == (receipt, identity)
    assert len(peer.commands) == command_count
    (rootfs_output / "unrelated.txt").write_bytes(b"unrelated")
    assert build()[2:] == (receipt, identity)
    assert len(peer.commands) == command_count

    initramfs.write_bytes(b"boot archive b")
    changed = build()
    assert changed[2]["recipe"] != receipt["recipe"]
    assert changed[3] != identity
    assert len(peer.commands) == command_count + 3

    for forbidden in (
        "CONFIG_MTD=y",
        "CONFIG_UBI=y",
        "CONFIG_SCSI=y",
        "CONFIG_ATA=y",
        "CONFIG_NVME_CORE=y",
        *additional_forbidden,
    ):
        peer.forbidden_config = forbidden
        initramfs.write_bytes(forbidden.encode())
        with pytest.raises(SystemExit, match=f"kernel unexpectedly contains {forbidden}"):
            build()


@pytest.mark.parametrize(
    ("target", "profile", "make_target"),
    [
        ("nokia-ta1618", "microsd-uboot", "zImage"),
        ("xiaomi-qin-1s-plus", "microsd-root", "Image"),
    ],
)
@pytest.mark.parametrize(
    ("environment_recipe", "environment_content"),
    [
        pytest.param("4" * 64, "3" * 64, id="changed-image-recipe"),
        pytest.param("2" * 64, "4" * 64, id="changed-installed-content"),
    ],
)
def test_external_root_rebuilds_for_another_environment(  # noqa: PLR0913
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    target: str,
    profile: str,
    make_target: str,
    environment_recipe: str,
    environment_content: str,
) -> None:
    """Changed tools rebuild the fixed output; the same environment then reuses it."""
    target_config = load_target(target, profile)
    platform = load_platform(target_config["platform"])
    assert target_config["linux"]["root"]["kind"] == "external"
    source = tmp_path / "linux"
    (source / "scripts").mkdir(parents=True)
    (source / "scripts/config").write_text("external Kbuild command peer\n")
    output = tmp_path / "work/kernel"
    rootfs_output = tmp_path / "rootfs"
    rootfs_output.mkdir()
    rootfs = rootfs_output / "rootfs.cpio"
    rootfs.write_bytes(b"unchanged external root")
    prepared = PreparedLinuxState("a" * 64, "b" * 64)
    peer = KernelCommandPeer(
        source,
        output,
        platform["linux"]["image_output"],
        str(Path(platform["linux"]["dtb_output_directory"]) / target_config["linux"]["dtb"]),
    )
    monkeypatch.setattr(process_build, "run", peer.run)
    monkeypatch.setattr(linux_state, "require_prepared_linux", lambda *_args: prepared)

    def ignore_validator(*_args: object) -> None:
        pass

    for name in (
        "verify_dtb_kconfig",
        "verify_target_identity",
        "verify_root_bootargs",
        "verify_profile_dtb_layout",
        "verify_builtin_audio_profile",
        "verify_builtin_panel_firmware",
    ):
        monkeypatch.setattr(kernel_build, name, ignore_validator)
    monkeypatch.setattr(kernel_build, "panel_firmware_kconfig_arguments", lambda *_args: [])

    def select_environment(recipe: str, content: str) -> None:
        monkeypatch.setenv("FPLINUX_ENVIRONMENT_RECIPE", recipe)
        monkeypatch.setenv("FPLINUX_ENVIRONMENT_CONTENT", content)
        monkeypatch.setenv(
            "FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE",
            environment_artifact_recipe_digest(recipe, content),
        )

    def build() -> tuple[Path, Path, dict[str, str], str]:
        return kernel_build.build_kernel(
            target,
            target_config,
            platform,
            bootstrap_recipe="f" * 64,
            linux_source=source,
            prepared_linux=prepared,
            linux_base="1" * 64,
            output=output,
            cross=platform["linux"]["cross_compile"],
            rootfs=rootfs,
            rootfs_output=rootfs_output,
            rootfs_recipe="d" * 64,
            audio_profile_firmware=None,
            jobs=2,
        )

    select_environment("2" * 64, "3" * 64)
    image, _dtb, first_receipt, first_identity = build()
    assert peer.commands[-1][-2:] == [make_target, "dtbs"]
    first_image = image.read_bytes()
    first_command_count = len(peer.commands)

    select_environment(environment_recipe, environment_content)
    changed_image, _dtb, changed_receipt, changed_identity = build()
    assert changed_image == image
    assert len(peer.commands) > first_command_count
    assert changed_image.read_bytes() != first_image
    assert changed_receipt["recipe"] != first_receipt["recipe"]
    assert changed_identity != first_identity

    command_count = len(peer.commands)
    image_bytes = changed_image.read_bytes()
    assert build()[2:] == (changed_receipt, changed_identity)
    assert len(peer.commands) == command_count
    assert changed_image.read_bytes() == image_bytes

# SPDX-License-Identifier: GPL-2.0-only
"""Exercise normalized kernel build plans with Kbuild and DT validation stand-ins."""

from __future__ import annotations

import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import pytest
from fplinux_cli.alpine import rootfs_state
from fplinux_cli.build import process as process_build
from fplinux_cli.build.display import render_linux_display_files
from fplinux_cli.build.kernel import compile as kernel_build
from fplinux_cli.build.kernel import state as linux_state
from fplinux_cli.build.kernel.state import PreparedLinuxState
from fplinux_cli.environment.definition import environment_artifact_recipe_digest
from fplinux_cli.manifests.kernel import kconfig_values
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target

from tests.fdt import binary_tree


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
        if command[0] == "fdtput":
            subprocess.run(command, check=True, timeout=10)
            return
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
            values = kconfig_values((self.output / ".config").read_text())
            version = values["CONFIG_LOCALVERSION"]
            firmware_directory = Path(values.get("CONFIG_EXTRA_FIRMWARE_DIR", '""').strip('"'))
            firmware_names = values.get("CONFIG_EXTRA_FIRMWARE", '""').strip('"').split()
            firmware = b"".join(
                (firmware_directory / name).read_bytes() for name in firmware_names
            )
            for relative, content in (
                (self.image, b"kernel " + version.encode()),
                (
                    self.dtb,
                    binary_tree(
                        [],
                        [
                            (
                                "chosen",
                                [
                                    ("linux,initrd-start", bytes([0xE5]) * 4),
                                    ("linux,initrd-end", bytes([0xF6]) * 4),
                                ],
                                [],
                            )
                        ],
                    ),
                ),
                ("vmlinux", b"kernel symbols" + firmware),
                ("System.map", b"kernel map"),
            ):
                path = self.output / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)


@pytest.mark.parametrize(
    ("target", "delivery", "make_target", "additional_forbidden", "boot_font", "font_symbol"),
    [
        pytest.param(
            "nokia-110-4g",
            "external",
            "zImage",
            (),
            "6x8",
            "CONFIG_FONT_6x8",
            id="ums-external-native-font",
        ),
        pytest.param(
            "nokia-110-4g",
            "external",
            "zImage",
            (),
            "7x14",
            "CONFIG_FONT_7x14",
            id="ums-external-alternate-font",
        ),
        pytest.param(
            "xiaomi-qin-1s-plus",
            "external",
            "Image",
            (),
            "7x14",
            "CONFIG_FONT_7x14",
            id="qin-external-ram",
        ),
        pytest.param(
            "inoi-240-modern-4g",
            "external",
            "zImage",
            (),
            "6x8",
            "CONFIG_FONT_6x8",
            id="inoi-240-storage-guards",
        ),
        pytest.param(
            "inoi-244-modern-4g",
            "external",
            "zImage",
            (),
            "7x14",
            "CONFIG_FONT_7x14",
            id="inoi-244-storage-guards",
        ),
        pytest.param(
            "maxvi-k15n-4g",
            "external",
            "zImage",
            (),
            "7x14",
            "CONFIG_FONT_7x14",
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
    for relative, contents in render_linux_display_files(target, target_config["display"]).items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
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
    ]

    def ignore_validator(*_args: object) -> None:
        pass

    for name in validators:
        monkeypatch.setattr(kernel_build, name, ignore_validator)

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
            board_output=tmp_path,
            cross=platform["linux"]["cross_compile"],
            rootfs_output=rootfs_output,
            rootfs_recipe="d" * 64,
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
    } == ({"CONFIG_FONT_6x8", "CONFIG_FONT_7x14"} if make_target == "zImage" else {font_symbol})
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
    assert changed[2] == receipt
    assert changed[3] != identity
    assert len(peer.commands) == command_count

    forbidden_config = (
        "CONFIG_MTD=y",
        "CONFIG_UBI=y",
        "CONFIG_SCSI=y",
        "CONFIG_ATA=y",
        "CONFIG_NVME_CORE=y",
        *additional_forbidden,
    )
    for index, forbidden in enumerate(forbidden_config, start=1):
        peer.forbidden_config = forbidden
        monkeypatch.setenv("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE", f"{index:064x}")
        with pytest.raises(SystemExit, match=f"kernel unexpectedly contains {forbidden}"):
            build()


@pytest.mark.parametrize(
    ("target", "profile", "make_target", "firmware_names"),
    [
        (
            "nokia-ta1618",
            "microsd-uboot",
            "zImage",
            "",
        ),
        (
            "xiaomi-qin-1s-plus",
            "microsd-root",
            "Image",
            "xiaomi,qin1splus-gc9307.bin",
        ),
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
    firmware_names: str,
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
    for relative, contents in render_linux_display_files(target, target_config["display"]).items():
        path = source / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
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
    ):
        monkeypatch.setattr(kernel_build, name, ignore_validator)
    profile_bytes = b"independent admitted sound settings\0\xff"
    early_firmware = tmp_path / "early-firmware.cpio"
    early_firmware.write_bytes(profile_bytes)

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
            board_output=tmp_path,
            cross=platform["linux"]["cross_compile"],
            rootfs_output=rootfs_output,
            rootfs_recipe="d" * 64,
            early_firmware=early_firmware,
            jobs=2,
        )

    select_environment("2" * 64, "3" * 64)
    image, _dtb, first_receipt, first_identity = build()
    assert peer.commands[-1][-2:] == [make_target, "dtbs"]
    first_image = image.read_bytes()
    configuration = kconfig_values((output / ".config").read_text())
    assert configuration["CONFIG_EXTRA_FIRMWARE"] == f'"{firmware_names}"'
    assert profile_bytes not in (output / "vmlinux").read_bytes()
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

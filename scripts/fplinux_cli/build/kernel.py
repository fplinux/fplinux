# SPDX-License-Identifier: GPL-2.0-only
"""Build and validate the target kernel and embedded firmware."""

from __future__ import annotations

import mmap
import shutil
from pathlib import Path
from typing import Any

from fplinux_cli import alpine_state, common, firmware_inputs, kbuild_state, linux_state
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build.inputs import fail
from fplinux_cli.device_state import DeviceStateError, device_kernel_identity, localversion
from fplinux_cli.device_tree import (
    DeviceTreeError,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    verify_target_identity,
)
from fplinux_cli.kbuild_state import KbuildStateError
from fplinux_cli.linux_state import LinuxStateError, PreparedLinuxState
from fplinux_cli.manifests.kernel import compose_kernel_config, kconfig_values, kernel_config_paths


def profile_kconfig_actions(target_config: dict[str, Any]) -> tuple[list[str], list[str]]:
    """Return the profile-only scripts/config actions in declared order."""
    linux = target_config["linux"]
    enable = linux.get("config_enable", [])
    disable = linux.get("config_disable", [])
    if (
        not isinstance(enable, list)
        or not isinstance(disable, list)
        or not all(isinstance(symbol, str) and symbol for symbol in [*enable, *disable])
    ):
        fail("target profile Kconfig actions are invalid")
    return list(enable), list(disable)


def profile_kconfig_arguments(config_enable: list[str], config_disable: list[str]) -> list[str]:
    """Render declared CONFIG_* actions for the Linux scripts/config command."""
    arguments: list[str] = []
    for action, symbols in (("--enable", config_enable), ("--disable", config_disable)):
        for symbol in symbols:
            if not symbol.startswith("CONFIG_") or len(symbol) == len("CONFIG_"):
                fail("target profile Kconfig symbol is invalid")
            arguments.extend((action, symbol.removeprefix("CONFIG_")))
    return arguments


def assert_profile_kconfig(
    config: Path, config_enable: list[str], config_disable: list[str]
) -> None:
    """Require selected profile Kconfig values after dependency resolution."""
    values = kconfig_values(inputs_build.require_file(config).read_text())
    for symbol in config_enable:
        if values.get(symbol, "n") != "y":
            fail(f"profile did not enable {symbol}")
    for symbol in config_disable:
        if values.get(symbol, "n") != "n":
            fail(f"profile did not disable {symbol}")


def kernel_build_commands(
    kbuild: list[str],
    config_command: list[str],
    targets: list[str],
    jobs: int,
) -> list[list[str]]:
    """Return the exact ordered Kbuild argv used by both the plan and executor."""
    if jobs < 1:
        fail("Kbuild jobs must be positive")
    return [
        [*kbuild, "olddefconfig"],
        config_command,
        [*kbuild, "olddefconfig"],
        [*kbuild, f"-j{jobs}", *targets],
    ]


def audio_profile_kconfig_arguments(
    target: str,
    firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
) -> list[str]:
    """Embed the fitted audio profile only when its complete group is present."""
    if firmware is None:
        return []
    directory = firmware_inputs.snapshot_device_data_group_directory(
        common.ROOT,
        target,
        "audio-profile",
    )
    names = " ".join(item.destination for item in firmware)
    return [
        "--set-str",
        "EXTRA_FIRMWARE",
        names,
        "--set-str",
        "EXTRA_FIRMWARE_DIR",
        str(directory),
    ]


def audio_profile_implementation(
    target: str,
    firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
) -> list[tuple[str, Path]]:
    """Expose exact fitted-profile bytes to the Kbuild causal-input receipt."""
    if firmware is None:
        return []
    records = []
    for item in firmware:
        relative = firmware_inputs.snapshot_device_data_path(
            target,
            "audio-profile",
            item.destination,
        )
        records.append((relative, common.ROOT / relative))
    return records


def _file_contains(path: Path, contents: bytes) -> bool:
    """Search a built artifact without copying the complete file into host memory."""
    with (
        path.open("rb") as stream,
        mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image,
    ):
        return image.find(contents) >= 0


def verify_builtin_audio_profile(
    target: str,
    config_path: Path,
    vmlinux: Path,
    firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
) -> None:
    """Require the configured fitted-profile names and exact bytes in vmlinux."""
    if firmware is None:
        return
    directory = firmware_inputs.snapshot_device_data_group_directory(
        common.ROOT,
        target,
        "audio-profile",
    )
    names = " ".join(item.destination for item in firmware)
    config_text = config_path.read_text()
    for expected in (
        f'CONFIG_EXTRA_FIRMWARE="{names}"',
        f'CONFIG_EXTRA_FIRMWARE_DIR="{directory}"',
    ):
        if expected not in config_text.splitlines():
            fail("kernel configuration lost the built-in audio-profile group")
    for item in firmware:
        if not _file_contains(vmlinux, item.contents):
            fail(f"kernel artifact does not embed audio-profile input {item.destination}")


def build_kernel(  # noqa: PLR0913 -- build inputs and causal receipts stay explicit.
    target: str,
    target_config: dict[str, Any],
    platform: dict[str, Any],
    *,
    bootstrap_recipe: str,
    linux_source: Path,
    prepared_linux: PreparedLinuxState,
    linux_base: str,
    output: Path,
    cross: str,
    rootfs: Path,
    rootfs_output: Path,
    rootfs_recipe: str,
    audio_profile_firmware: tuple[firmware_inputs.FirmwareInput, ...] | None,
    jobs: int,
) -> tuple[Path, Path, dict[str, str], str]:
    """Build or exactly reuse zImage and the declared target DTB in ``work/kernel``."""
    try:
        work = output.parent
        base, fragment = kernel_config_paths(target, target_config, platform)
        work.mkdir(parents=True, exist_ok=True)
        defconfig = work / "kernel.defconfig"
        defconfig.write_bytes(compose_kernel_config(base, fragment))
        root_contract = target_config["linux"]["root"]
        initramfs_record: dict[str, int | str] | None = None
        initramfs_input: Path | None = None
        initramfs_receipt: dict[str, str] | None = None
        if root_contract["kind"] == "initramfs":
            initramfs_record = kbuild_state.initramfs_identity(rootfs)
            initramfs_input = kbuild_state.initramfs_input_path(work, initramfs_record)
            initramfs_receipt = alpine_state.trusted_receipt_identity(rootfs_output, rootfs_recipe)
            device_root: dict[str, object] = {
                "kind": "initramfs",
                "artifact": initramfs_record,
                "receipt": initramfs_receipt,
            }
        elif root_contract["kind"] == "external":
            device_root = root_contract
        else:
            fail(f"unsupported Linux root kind: {root_contract['kind']}")
        kbuild = [
            "make",
            "-C",
            str(linux_source),
            f"O={output}",
            f"ARCH={platform['linux']['arch']}",
            f"CROSS_COMPILE={cross}",
        ]
        config_script = inputs_build.require_file(
            linux_source / platform["linux"]["config_script"]
        )
        current_linux = linux_state.require_prepared_linux(linux_source, prepared_linux)
        implementation = [
            (
                "scripts/fplinux_cli/build_env.py",
                inputs_build.root_source("scripts/fplinux_cli/build_env.py"),
            ),
            *inputs_build.implementation_sources(),
            (
                "scripts/fplinux_cli/device_state.py",
                inputs_build.root_source("scripts/fplinux_cli/device_state.py"),
            ),
            ("scripts/fplinux_cli/kbuild_state.py", Path(kbuild_state.__file__)),
        ]
        implementation.extend(audio_profile_implementation(target, audio_profile_firmware))
        config_enable, config_disable = profile_kconfig_actions(target_config)
        device_identity = device_kernel_identity(
            target=target,
            linux_recipe=current_linux.linux_recipe,
            bootstrap_recipe=bootstrap_recipe,
            root=device_root,
            kbuild_implementation=kbuild_state.implementation_identity(implementation),
            arch=platform["linux"]["arch"],
            defconfig=defconfig,
            dtb=target_config["linux"]["dtb"],
            profile=inputs_build.selected_profile(target_config),
            config_enable=config_enable,
            config_disable=config_disable,
        )
        initramfs_source = str(initramfs_input) if initramfs_input is not None else ""
        config_command = [
            str(config_script),
            "--file",
            str(output / ".config"),
            "--set-str",
            "INITRAMFS_SOURCE",
            initramfs_source,
            "--set-str",
            "LOCALVERSION",
            localversion(device_identity),
            *audio_profile_kconfig_arguments(target, audio_profile_firmware),
            *profile_kconfig_arguments(config_enable, config_disable),
        ]
        commands = kernel_build_commands(
            kbuild,
            config_command,
            platform["linux"]["targets"],
            jobs,
        )
        output_paths = (
            platform["linux"]["image_output"],
            str(Path(platform["linux"]["dtb_output_directory"]) / target_config["linux"]["dtb"]),
            "vmlinux",
            "System.map",
            ".config",
        )
        plan = kbuild_state.create_plan(
            linux_recipe=current_linux.linux_recipe,
            linux_base=inputs_build.require_sha256(linux_base, "Linux base source"),
            defconfig=defconfig,
            defconfig_path="generated/kernel.defconfig",
            root=root_contract,
            initramfs=initramfs_record,
            initramfs_input=initramfs_input,
            initramfs_receipt=initramfs_receipt,
            arch=platform["linux"]["arch"],
            cross_compile=cross,
            commands=commands,
            outputs=output_paths,
            implementation=implementation,
        )
        if kbuild_state.cache_hit(work, output, plan):
            process_build.log_message(f"Kbuild causal receipt hit: {plan.recipe[:16]}")
        else:
            kbuild_state.discard_success_receipt(work)
            kbuild_state.prepare_output(work, output)
            if initramfs_record is not None:
                kbuild_state.materialize_initramfs_input(work, rootfs, plan)
            shutil.copyfile(defconfig, output / ".config")
            for command in commands[:3]:
                process_build.run(command)
            assert_profile_kconfig(output / ".config", config_enable, config_disable)
            process_build.run(commands[3])

        zimage = inputs_build.require_file(output / platform["linux"]["image_output"])
        vmlinux = inputs_build.require_file(output / "vmlinux")
        dtb = inputs_build.require_file(
            output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
        )
        try:
            verify_target_identity(
                dtb,
                target,
                target_config["identity"]["display_name"],
                (
                    target_config["identity"]["compatible"],
                    platform["identity"]["compatible"],
                ),
            )
            verify_root_bootargs(dtb, root_contract)
            layout = target_config.get("layout")
            if isinstance(layout, dict):
                verify_profile_dtb_layout(dtb, layout, target_config["linux"]["memory"])
        except DeviceTreeError as error:
            fail(str(error))
        config_text = inputs_build.require_file(output / ".config").read_text()
        assert_profile_kconfig(output / ".config", config_enable, config_disable)
        verify_builtin_audio_profile(
            target,
            output / ".config",
            vmlinux,
            audio_profile_firmware,
        )
        for forbidden in target_config["linux"]["forbidden_config"]:
            if forbidden in config_text:
                fail(f"kernel unexpectedly contains {forbidden}")
        if not kbuild_state.cache_hit(work, output, plan):
            kbuild_state.publish_success(work, output, plan)
        return zimage, dtb, kbuild_state.receipt_identity(work, output, plan), device_identity
    except (DeviceStateError, KbuildStateError, LinuxStateError) as error:
        fail(str(error))

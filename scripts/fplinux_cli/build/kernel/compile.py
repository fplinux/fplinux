# SPDX-License-Identifier: GPL-2.0-only
"""Execute and verify a kernel build using its exact causal receipt."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.alpine import rootfs_state as alpine_state
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    verify_dtb_kconfig,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    verify_target_identity,
)
from fplinux_cli.common import fail
from fplinux_cli.manifests.kernel import kconfig_values

from . import receipts as kbuild_state
from . import state as linux_state
from .configuration import (
    assert_build_type_kconfig,
    assert_profile_kconfig,
    audio_profile_implementation,
    audio_profile_kconfig_arguments,
    prepare_kernel_config,
    profile_kconfig_actions,
    profile_kconfig_arguments,
    verify_builtin_audio_profile,
)
from .identity import DeviceStateError, device_kernel_identity, localversion
from .receipts import KbuildStateError
from .state import LinuxStateError, PreparedLinuxState

if TYPE_CHECKING:
    from fplinux_cli.device_data import inputs as firmware_inputs


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
        config_command,
        [*kbuild, "olddefconfig"],
        [*kbuild, f"-j{jobs}", *targets],
    ]


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
        defconfig, config_paths = prepare_kernel_config(target, target_config, platform, work)
        root_contract = target_config["linux"]["root"]
        initramfs_record: dict[str, int | str] | None = None
        initramfs_input: Path | None = None
        initramfs_receipt: dict[str, str] | None = None
        if root_contract["kind"] == "initramfs":
            kernel_rootfs = inputs_build.require_file(
                rootfs.with_name(alpine_state.INITRAMFS_NAME)
            )
            initramfs_record = kbuild_state.initramfs_identity(kernel_rootfs)
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
            f"CC=ccache {cross}gcc",
        ]
        config_script = inputs_build.require_file(
            linux_source / platform["linux"]["config_script"]
        )
        current_linux = linux_state.require_prepared_linux(linux_source, prepared_linux)
        implementation = inputs_build.kernel_implementation_sources()
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
            build_type=target_config["build_type"],
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
            linux_state.write_profile_root(output, target_config)
            if initramfs_record is not None:
                kbuild_state.materialize_initramfs_input(work, kernel_rootfs, plan)
            shutil.copyfile(defconfig, output / ".config")
            ccache_environment = {
                "CCACHE_DIR": str(inputs_build.CACHE / "ccache"),
                "CCACHE_MAXSIZE": "1GiB",
                "CCACHE_BASEDIR": str(inputs_build.OUTPUT),
                "CCACHE_NAMESPACE": os.environ["FPLINUX_CONTAINER_IMAGE_RECIPE"],
            }
            for command in commands[:-1]:
                process_build.run(command, environment=ccache_environment)
            assert_profile_kconfig(output / ".config", config_enable, config_disable)
            assert_build_type_kconfig(
                output / ".config", config_paths[-1], target_config["build_type"]
            )
            process_build.run(commands[-1], environment=ccache_environment)

        zimage = inputs_build.require_file(output / platform["linux"]["image_output"])
        vmlinux = inputs_build.require_file(output / "vmlinux")
        dtb = inputs_build.require_file(
            output / platform["linux"]["dtb_output_directory"] / target_config["linux"]["dtb"]
        )
        config_text = inputs_build.require_file(output / ".config").read_text()
        try:
            verify_dtb_kconfig(
                dtb, kconfig_values(config_text), platform["linux"]["dt_config_checks"]
            )
            process_build.log_message("DTB/Kconfig consistency verified")
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
        assert_profile_kconfig(output / ".config", config_enable, config_disable)
        assert_build_type_kconfig(
            output / ".config", config_paths[-1], target_config["build_type"]
        )
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

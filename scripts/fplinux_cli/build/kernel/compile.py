# SPDX-License-Identifier: GPL-2.0-only
"""Execute and verify a kernel build using its exact causal receipt."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Any

from fplinux_cli.alpine import rootfs_state as alpine_state
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build.device_tree import (
    DeviceTreeError,
    verify_dtb_kconfig,
    verify_profile_dtb_layout,
    verify_root_bootargs,
    verify_target_identity,
    write_boot_dtb,
)
from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes, sha256_file
from fplinux_cli.manifests.kernel import kconfig_values
from fplinux_cli.manifests.values import sha256_value
from fplinux_cli.runtime.connection import device_identity

from . import receipts as kbuild_state
from . import state as linux_state
from .configuration import (
    assert_build_type_kconfig,
    prepare_builtin_firmware,
    prepare_kernel_config,
)
from .identity import DeviceStateError, device_kernel_identity, localversion
from .receipts import KbuildStateError
from .state import LinuxStateError, PreparedLinuxState


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
    rootfs_output: Path,
    rootfs_recipe: str,
    jobs: int,
    board_output: Path,
    early_firmware: Path | None = None,
) -> tuple[Path, Path, dict[str, str], str]:
    """Build or exactly reuse the declared kernel image and DTB in ``work/kernel``."""
    try:
        work = output.parent
        defconfig, config_paths = prepare_kernel_config(target, target_config, platform, work)
        root_contract = target_config["linux"]["root"]
        initramfs_delivery = target_config["bootstrap"]["initramfs_delivery"]
        if root_contract["kind"] == "initramfs":
            kernel_rootfs = inputs_build.require_file(rootfs_output / alpine_state.INITRAMFS_NAME)
            initramfs_record = {
                "sha256": sha256_file(kernel_rootfs),
                "size": kernel_rootfs.stat().st_size,
            }
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
        builtin_firmware = prepare_builtin_firmware(target_config, source=linux_source, work=work)
        implementation = inputs_build.kernel_implementation_sources()
        implementation.extend(builtin_firmware.implementation())
        environment_recipe = os.environ["FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE"]
        implementation_recipe = kbuild_state.implementation_identity(implementation)
        board_recipe = device_kernel_identity(
            target=target,
            linux_recipe=current_linux.linux_recipe,
            bootstrap_recipe=bootstrap_recipe,
            environment_recipe=environment_recipe,
            root=device_root,
            initramfs_delivery=initramfs_delivery,
            kbuild_implementation=implementation_recipe,
            arch=platform["linux"]["arch"],
            defconfig=defconfig,
            dtb=target_config["linux"]["dtb"],
            profile=inputs_build.selected_profile(target_config),
            build_type=target_config["build_type"],
        )
        binary_recipe = sha256_bytes(
            canonical_json_bytes(
                {
                    "linux": current_linux.tree_recipe,
                    "environment": environment_recipe,
                    "implementation": implementation_recipe,
                    "config": sha256_file(defconfig),
                }
            )
        )
        kernel_localversion = localversion(binary_recipe)
        early_firmware_digest = sha256_file(early_firmware) if early_firmware is not None else ""
        boot_recipe = sha256_bytes(
            f"{board_recipe}\n{rootfs_recipe}\n{early_firmware_digest}\n".encode("ascii")
        )
        config_command = [
            str(config_script),
            "--file",
            str(output / ".config"),
            "--set-str",
            "INITRAMFS_SOURCE",
            "",
            "--set-str",
            "LOCALVERSION",
            kernel_localversion,
            *builtin_firmware.kconfig_arguments(),
        ]
        commands = kernel_build_commands(
            kbuild,
            config_command,
            platform["linux"]["targets"],
            jobs,
        )
        board_dtb = str(
            Path(platform["linux"]["dtb_output_directory"]) / target_config["linux"]["dtb"]
        )
        output_paths = (
            platform["linux"]["image_output"],
            "vmlinux",
            "System.map",
            ".config",
        )
        plan = kbuild_state.create_plan(
            linux_recipe=current_linux.tree_recipe,
            linux_base=sha256_value(linux_base, "Linux base source"),
            defconfig=defconfig,
            defconfig_path="generated/kernel.defconfig",
            arch=platform["linux"]["arch"],
            cross_compile=cross,
            commands=commands,
            outputs=output_paths,
            implementation=implementation,
        )
        ccache_environment = {
            "CCACHE_DIR": str(inputs_build.CACHE / "ccache"),
            "CCACHE_MAXSIZE": "1GiB",
            "CCACHE_BASEDIR": str(inputs_build.OUTPUT),
            "CCACHE_NAMESPACE": environment_recipe,
        }
        if kbuild_state.cache_hit(work, output, plan):
            process_build.log_message(f"Kbuild causal receipt hit: {plan.recipe[:16]}")
        else:
            kbuild_state.discard_success_receipt(work)
            kbuild_state.prepare_output(work, output)
            linux_state.write_default_bootargs(output)
            shutil.copyfile(defconfig, output / ".config")
            for command in commands[:-1]:
                process_build.run(command, environment=ccache_environment)
            assert_build_type_kconfig(
                output / ".config", config_paths[-1], target_config["build_type"]
            )
            process_build.run(commands[-1], environment=ccache_environment)

        zimage = inputs_build.require_file(output / platform["linux"]["image_output"])
        vmlinux = inputs_build.require_file(output / "vmlinux")
        if not (output / board_dtb).is_file():
            process_build.run([*kbuild, f"-j{jobs}", "dtbs"], environment=ccache_environment)
        try:
            dtb = write_boot_dtb(
                inputs_build.require_file(output / board_dtb),
                board_output,
                target_config,
                boot_recipe,
            )
        except DeviceTreeError as error:
            fail(str(error))
        config_text = inputs_build.require_file(output / ".config").read_text()
        try:
            verify_dtb_kconfig(
                dtb, kconfig_values(config_text), target_config["linux"]["dt_config_checks"]
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
        assert_build_type_kconfig(
            output / ".config", config_paths[-1], target_config["build_type"]
        )
        builtin_firmware.verify(output / ".config", vmlinux)
        for forbidden in target_config["linux"]["forbidden_config"]:
            if forbidden in config_text:
                fail(f"kernel unexpectedly contains {forbidden}")
        if not kbuild_state.cache_hit(work, output, plan):
            kbuild_state.publish_success(work, output, plan)
        return (
            zimage,
            dtb,
            kbuild_state.receipt_identity(work, output, plan),
            device_identity(kernel_localversion, boot_recipe),
        )
    except (DeviceStateError, KbuildStateError, LinuxStateError) as error:
        fail(str(error))

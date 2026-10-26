# SPDX-License-Identifier: GPL-2.0-only
"""Collect the fixed-phone preparation runtime for an autonomous archive."""

from __future__ import annotations

import io
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from textwrap import dedent
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import ZIP_TIMESTAMP, canonical_json_bytes, fail, sha256_bytes
from fplinux_cli.device_data.prepare import NAND_ECC_TOOL
from fplinux_cli.manifests.identity import target_display_name
from fplinux_cli.packaging.readme import preparation_instructions, shutdown_instructions
from fplinux_cli.runtime.windows_tools import windows_tool_paths


@dataclass(frozen=True)
class AutonomousPayload:
    """Additional archive members, native runtime inputs and executable ZIP modes."""

    files: dict[str, bytes]
    executables: set[str]


def _readme(config: dict[str, Any], *, target: str, host: str, preparation: bool) -> bytes:
    name = target_display_name(config["identity"])
    emmc = config.get("emmc") is not None
    saved_data = "original settings" if emmc else "backup"
    instructions = config["adapter"]["boot_instructions"]
    shutdown = shutdown_instructions(
        external_root=config["linux"]["root"]["kind"] == "external",
        with_offline_documentation=False,
    )
    if host == "windows":
        host_instructions = dedent("""\
            Use a Windows 10 or 11 x64 PC. Python and the tools needed to connect
            to the phone are included. Run the launcher as your regular Windows user.
            It may ask to install a USB driver. Read the included driver license
            and approve the administrator prompt if you agree to install it.
            If driver setup asks you to restart the launcher, open Run.cmd again.
            """).removesuffix("\n")
        if config.get("platform") == "sc9820e":
            host_instructions += (
                "\nUSB loading and SSH from Windows have not been verified with this phone."
            )
        launch_instructions = "Double-click Run.cmd in the extracted folder."
        dump_command = 'Run.cmd --from-dump "C:\\path\\to\\nand.bin"'
        saved_files_instructions = (
            f"The phone's {saved_data} and prepared data are saved on the PC in:\n"
            f"  %LOCALAPPDATA%\\FPLinux\\phones\\{target}"
        )
    else:
        host_instructions = dedent("""\
            Use a Linux x86-64 PC with coreutils, iproute2, udev and a network manager
            that automatically runs IPv4 DHCP on new USB interfaces, such as
            NetworkManager.
            Python and OpenSSH are included. For the first USB setup, sudo must be
            available. The launcher asks for administrator access if the included
            phone USB access rules are not installed.
            """).removesuffix("\n")
        launch_instructions = "Open a terminal in the extracted folder and run:\n  sh Run.sh"
        dump_command = "sh Run.sh --from-dump /path/to/nand.bin"
        saved_files_instructions = (
            f"The phone's {saved_data} and prepared data are saved on the PC in:\n"
            f"  ~/.local/share/fplinux/phones/{target}\n"
            "If XDG_DATA_HOME is set, its directory replaces ~/.local/share."
        )
    introduction = (
        "This archive is only for {name}. RAM boot does not access internal storage."
        if config.get("platform") == "sc9820e" and not emmc
        else "This archive is only for {name}. Internal storage is read only."
    ).format(name=name)
    sections = [
        dedent("""\
            FPLinux - {name}

            {introduction}
            FPLinux runs in the phone's temporary memory (RAM). Files kept only
            in RAM are lost when the phone loses power.
            """)
        .removesuffix("\n")
        .format(name=name, introduction=introduction),
        dedent("""\
            Before you start

            Extract the entire ZIP into a folder on your PC. Keep all its files
            together; do not open the launcher from inside the ZIP. Use this folder
            with one physical phone and a USB data cable. Only one launcher can
            run from this folder at a time.

            {host_instructions}
            """)
        .removesuffix("\n")
        .format(host_instructions=host_instructions),
        dedent("""\
            Start FPLinux

            1. Turn the phone off and disconnect its USB cable.
            2. {launch_instructions}
            3. Wait for the launcher's invitation to connect the phone, then follow
               these instructions:

            {instructions}

            Keep USB connected and follow the messages in this window.
            Ctrl+C cancels loading or waiting; it does not power the phone off.
            """)
        .removesuffix("\n")
        .format(launch_instructions=launch_instructions, instructions=instructions),
    ]
    if preparation:
        sections.append(preparation_instructions(emmc=emmc).removesuffix("\n"))
    sections.extend(
        [
            dedent("""\
                Use FPLinux and reconnect

                Once FPLinux is running, the launcher opens SSH: a command-line
                terminal on your PC where you can enter phone commands.

                Type exit or press Ctrl+D to close the phone terminal. The phone
                keeps running. To reconnect, leave it powered and connected by USB,
                then open the same launcher from the same folder again.
                """).removesuffix("\n"),
            shutdown.removesuffix("\n"),
            dedent("""\
                Help and saved files

                Each launch saves runtime.log in a new folder inside logs and
                displays its exact path. Include that runtime.log when asking for
                help. Private connection keys and details needed to reconnect are
                saved in data/session; keep them private and keep this folder for
                the same physical phone.
                """).removesuffix("\n"),
        ]
    )
    if preparation and emmc:
        if host == "windows":
            dump_command = 'Run.cmd --from-dump "C:\\path\\to\\phone-user-area.bin"'
        else:
            dump_command = "sh Run.sh --from-dump /path/to/phone-user-area.bin"
        sections.extend(
            [
                dedent("""\
                    {saved_files_instructions}
                    This folder holds the selected original files in originals/ and
                    prepared phone data in phone/. Keep it for this physical phone.
                    Add --phone-data PATH to the launch command to choose a separate
                    folder, for example when using another handset. PATH is used
                    exactly as given.
                    """)
                .removesuffix("\n")
                .format(saved_files_instructions=saved_files_instructions),
                dedent("""\
                    Use a saved eMMC image

                    To read the selected settings from a complete eMMC user-area image
                    from this same phone, run this command from the extracted folder:
                      {dump_command}
                    The launcher reads the image in place, saves only the selected
                    original files and prepares the phone data. It then continues
                    loading FPLinux. Connect the phone only when the launcher asks.
                    """)
                .removesuffix("\n")
                .format(dump_command=dump_command),
            ]
        )
    elif preparation:
        sections.extend(
            [
                dedent("""\
                    {saved_files_instructions}
                    This folder holds the internal-storage backup nand.bin, its
                    matching nand.bin.json file, and prepared phone data in phone/.
                    Compatible backup and prepared data can be reused by other
                    FPLinux archives for this same physical phone.
                    Add --phone-data PATH to the launch command to choose a separate
                    folder, for example when using another handset. PATH is used
                    exactly as given.
                    """)
                .removesuffix("\n")
                .format(saved_files_instructions=saved_files_instructions),
                dedent("""\
                    Use an existing backup

                    To use a complete internal-storage backup from this same phone,
                    run this command from the extracted folder:
                      {dump_command}
                    Keep its matching nand.bin.json file beside nand.bin; the launcher
                    needs both files to check the backup. It preserves both input files,
                    copies the backup into the saved-data folder and prepares the phone
                    data if needed. It then continues loading FPLinux. Connect the phone
                    only when the launcher asks.
                    """)
                .removesuffix("\n")
                .format(dump_command=dump_command),
            ]
        )
    text = "\n\n".join(sections) + "\n"
    return text.encode("utf-8")


def _linux_ecc_sources(platform: str) -> dict[str, bytes]:
    """Deliver the verified source projection and license for the NAND corrector."""
    from fplinux_cli.build.host_tools.host import (  # noqa: PLC0415
        copy_host_project_files,
        extract_host_members,
    )
    from fplinux_cli.build.sources import source_lock_entry  # noqa: PLC0415
    from fplinux_cli.manifests.platforms import load_platform  # noqa: PLC0415
    from fplinux_cli.manifests.source_catalog import acquire_source  # noqa: PLC0415

    recipe = next(
        item for item in load_platform(platform)["host"]["tools"] if item["name"] == NAND_ECC_TOOL
    )
    sources = common.load_toml(common.ROOT / "sources.lock.toml")
    lock = source_lock_entry(sources, recipe["source_lock"])
    cache = common.ROOT / ".cache"
    archive = acquire_source(common.ROOT, recipe["source_lock"], cache)
    payload = io.BytesIO()
    with tempfile.TemporaryDirectory(dir=cache, prefix=".autonomous-ecc-source-") as temporary:
        source = Path(temporary) / recipe["source_directory"]
        prefix = recipe["archive_prefix"].replace("{commit}", lock["commit"])
        extract_host_members(archive, prefix, recipe["members"], lock["files"], source)
        copy_host_project_files(source, recipe["copies"])
        license_contents = (source / "COPYING").read_bytes()
        with zipfile.ZipFile(payload, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for path in sorted(source.iterdir()):
                member = zipfile.ZipInfo(f"{NAND_ECC_TOOL}/{path.name}", ZIP_TIMESTAMP)
                member.compress_type = zipfile.ZIP_DEFLATED
                output.writestr(member, path.read_bytes())
    return {
        "sources/ums9117-nand-ecc-source.zip": payload.getvalue(),
        "licenses/host-tools/ums9117-nand-ecc-COPYING.txt": license_contents,
    }


def autonomous_payload(
    config: dict[str, Any],
    *,
    host: str,
    runtime: dict[str, Any],
) -> AutonomousPayload:
    """Collect host dependencies; executable project code comes from the built bundle."""
    descriptor = runtime["device_data"]
    files = {
        "README.txt": _readme(
            config, target=runtime["target"], host=host, preparation=descriptor is not None
        )
    }
    platform = config["platform"]
    has_ecc = descriptor is not None and descriptor["ecc_module"] is not None
    executables: set[str] = set()
    if host == "windows":
        from fplinux_cli.build.host_tools.windows import prepare_windows_host  # noqa: PLC0415
        from fplinux_cli.build.host_tools.windows_driver import (  # noqa: PLC0415
            prepare_windows_driver,
        )

        dependencies = prepare_windows_host(
            cache=common.ROOT / ".cache", offline=True, platform=platform, nand_ecc=has_ecc
        )
        dependencies.update(prepare_windows_driver(cache=common.ROOT / ".cache", offline=True))
        for relative, source in dependencies.items():
            if relative in files:
                fail(f"duplicate Windows archive path: {relative}")
            if source.is_symlink() or not source.is_file():
                fail(f"Windows archive input is missing or invalid: {source}")
            files[relative] = source.read_bytes()
            if source.suffix.lower() == ".exe":
                executables.add(relative)
        files["windows-host.json"] = canonical_json_bytes(
            {
                "host_tools": windows_tool_paths(runtime["host_tools"]),
                "sha256": {
                    relative: sha256_bytes(data) for relative, data in sorted(files.items())
                },
            }
        )
    else:
        from fplinux_cli.build.host_tools.linux_host import prepare_linux_host  # noqa: PLC0415

        if platform == "ums9117":
            license_file = common.ROOT / "LICENSES/Unlicense.txt"
            if license_file.is_symlink() or not license_file.is_file():
                fail(f"host tool license is missing or invalid: {license_file}")
            files["licenses/host-tools/Unlicense.txt"] = license_file.read_bytes()
        if has_ecc:
            files.update(_linux_ecc_sources(platform))
        for relative, source in prepare_linux_host(
            cache=common.ROOT / ".cache", offline=True
        ).items():
            if relative in files:
                fail(f"duplicate Linux archive path: {relative}")
            files[relative] = source.read_bytes()
            if source.stat().st_mode & 0o111:
                executables.add(relative)
    return AutonomousPayload(files, executables)

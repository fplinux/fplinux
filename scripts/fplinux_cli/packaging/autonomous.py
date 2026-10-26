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
from fplinux_cli.packaging.readme import shutdown_instructions

WINDOWS_TOOLS = {
    "loader": "tools/spd_dump.exe",
    "bridge": "tools/libc_server.exe",
    "ssh": "tools/openssh/ssh.exe",
    "keygen": "tools/openssh/ssh-keygen.exe",
    "keyscan": "tools/openssh/ssh-keyscan.exe",
    "driver": "tools/driver/wdi-simple.exe",
}


@dataclass(frozen=True)
class AutonomousPayload:
    """Additional archive members, native runtime inputs and executable ZIP modes."""

    files: dict[str, bytes]
    runtime_files: set[str]
    executables: set[str]


def _readme(config: dict[str, Any], *, host: str, preparation: bool) -> bytes:
    name = target_display_name(config["identity"])
    instructions = config["adapter"]["boot_instructions"]
    shutdown = shutdown_instructions(
        external_root=config["linux"]["root"]["kind"] == "external",
        with_offline_documentation=False,
    )
    if host == "windows10":
        host_instructions = dedent("""\
            Use Windows 10 or 11 x64. Python and the host programs are bundled.
            On first use, Run can ask to install the phone's USB driver.
            Read the included driver license and approve the administrator prompt.
            Double-click Run.cmd to start or reconnect.
            """).removesuffix("\n")
        dump_command = 'Run.cmd --from-dump "C:\\path\\to\\nand.bin"'
    else:
        host_instructions = dedent("""\
            Use Linux x86-64 with coreutils, iproute2, udev and a network manager
            that automatically runs IPv4 DHCP on new USB interfaces, such as
            NetworkManager.
            Python and OpenSSH are bundled. Run asks for administrator access
            once if the included phone USB access rules are not installed.
            Open a terminal in this directory and run:
              sh Run.sh
            """).removesuffix("\n")
        dump_command = "sh Run.sh --from-dump /path/to/nand.bin"
    introduction = (
        "This archive is only for {name}. RAM boot does not access internal storage."
        if config.get("platform") == "sc9820e"
        else "This archive is only for {name}. Internal storage is read only."
    ).format(name=name)
    sections = [
        dedent("""\
            FPLinux - {name}

            {introduction}
            """)
        .removesuffix("\n")
        .format(name=name, introduction=introduction),
        host_instructions,
        dedent("""\
            For a fresh load, keep the phone disconnected until Run displays
            the connection instructions. Leave a running phone connected to reopen SSH.
            {instructions}
            """)
        .removesuffix("\n")
        .format(instructions=instructions),
    ]
    if preparation:
        sections.append(
            dedent("""\
                The first run saves a complete backup and prepares your phone's own data.
                When it finishes, release cards as described below before disconnecting USB.
                If the phone stays on, briefly remove and reinsert its battery. Run the same
                launcher again with the phone powered off to start FPLinux.
                Your NAND backup is data/nand.bin and prepared phone data is data/phone.

                To prepare from an existing complete NAND backup without connecting the phone:
                  {dump_command}
                This preparation preserves the backup and cannot replace existing data/phone.
                """)
            .removesuffix("\n")
            .format(dump_command=dump_command)
        )
    if config.get("platform") == "sc9820e":
        sections.append("The source-built RAM loaders are included in this archive.")
    sections.extend(
        [
            dedent("""\
                Run loads FPLinux into RAM and opens SSH. Type exit or press Ctrl+D to
                close SSH. Open the same launcher again to reconnect while the phone
                remains on.
                Only one launcher can run from this directory at a time.
                Ctrl+C cancels loading or waiting; it does not power the phone off.
                """).removesuffix("\n"),
            dedent("""\
                Keep this directory for this one phone. Private connection keys and
                session records are stored in data/session. Each launch writes a new
                log in logs and displays its exact path.
                """).removesuffix("\n"),
        ]
    )
    sections.append(shutdown.removesuffix("\n"))
    text = "\n\n".join(sections) + "\n"
    return text.encode("utf-8")


def _linux_ecc_sources(platform: str, *, offline: bool) -> dict[str, bytes]:
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
    archive = acquire_source(common.ROOT, recipe["source_lock"], cache, offline=offline)
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
    offline: bool,
    runtime: dict[str, Any],
) -> AutonomousPayload:
    """Collect host dependencies; executable project code comes from the built bundle."""
    direct_boot = config["platform"] == "sc9820e"
    if direct_boot and host != "linux":
        fail("SC9820E archives support Linux x86-64 only")
    descriptor = runtime["device_data"]
    files = {"README.txt": _readme(config, host=host, preparation=descriptor is not None)}
    platform = config["platform"]
    has_ecc = descriptor is not None and descriptor["ecc_module"] is not None
    executables: set[str] = set()
    if host == "windows10":
        from fplinux_cli.build.host_tools.windows import prepare_windows_host  # noqa: PLC0415
        from fplinux_cli.build.host_tools.windows_driver import (  # noqa: PLC0415
            prepare_windows_driver,
        )

        dependencies = prepare_windows_host(
            cache=common.ROOT / ".cache", offline=offline, platform=platform, nand_ecc=has_ecc
        )
        dependencies.update(prepare_windows_driver(cache=common.ROOT / ".cache", offline=offline))
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
                "host_tools": WINDOWS_TOOLS,
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
            files.update(_linux_ecc_sources(platform, offline=offline))
        for relative, source in prepare_linux_host(
            cache=common.ROOT / ".cache", offline=offline
        ).items():
            if relative in files:
                fail(f"duplicate Linux archive path: {relative}")
            files[relative] = source.read_bytes()
            if source.stat().st_mode & 0o111:
                executables.add(relative)
    runtime_files = {relative for relative in files if relative.startswith(("runtime/", "tools/"))}
    return AutonomousPayload(files, runtime_files, executables)

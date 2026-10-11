# SPDX-License-Identifier: GPL-2.0-only
"""Collect the fixed-phone preparation runtime for an autonomous archive."""

from __future__ import annotations

import io
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import ZIP_TIMESTAMP, canonical_json_bytes, fail, sha256_bytes
from fplinux_cli.device_data.prepare import NAND_ECC_MODULE, NAND_ECC_TOOL
from fplinux_cli.manifests.identity import target_display_name

WINDOWS_TOOLS = {
    "loader": "tools/spd_dump.exe",
    "bridge": "tools/libc_server.exe",
    "ssh": "tools/openssh/ssh.exe",
    "keygen": "tools/openssh/ssh-keygen.exe",
    "keyscan": "tools/openssh/ssh-keyscan.exe",
    "driver": "tools/driver/wdi-simple.exe",
}

_RUNTIME_PYTHON = (
    "__init__.py",
    "common.py",
    "device_data/__init__.py",
    "device_data/audio_profile.py",
    "device_data/cpio.py",
    "device_data/formats.py",
    "device_data/inputs.py",
    "device_data/partitions.py",
    "device_data/portable.py",
    "device_data/prepare.py",
    "manifests/__init__.py",
    "manifests/values.py",
    "runtime/__init__.py",
    "runtime/nand_backup.py",
)
_CM4_PYTHON = (
    "device_data/bluetooth_firmware.py",
    "device_data/cm4.py",
    "device_data/fitted.py",
    "device_data/fm_radio.py",
)


def runtime_python_files(config: dict[str, Any]) -> tuple[str, ...]:
    """Select the shared runtime and the phone's declared radio preparation."""
    selected = list(_RUNTIME_PYTHON)
    groups = config["device_data"]["groups"]
    if "bluetooth" in groups or "fm-radio" in groups:
        selected.extend(_CM4_PYTHON)
    if "wcn-firmware" in groups or "bluetooth" in groups:
        selected.append("device_data/running_nv.py")
    return tuple(f"scripts/fplinux_cli/{name}" for name in selected)


@dataclass(frozen=True)
class AutonomousPayload:
    """Additional archive members and their executable ZIP modes."""

    files: dict[str, bytes]
    executables: set[str]


def _source_bytes(relative: str) -> bytes:
    source = common.ROOT / relative
    if source.is_symlink() or not source.is_file():
        fail(f"autonomous package input is missing or invalid: {source}")
    return source.read_bytes()


def device_data_manifest(target: str, config: dict[str, Any]) -> bytes:
    """Declare only the selected phone, including its separately extracted board maps."""
    declarations = config["device_data"]
    groups = {
        **declarations["groups"],
        "board-maps": [
            {"source": "pinmap.bin", "destination": "pinmap.bin"},
            {"source": "keymap.bin", "destination": "keymap.bin"},
        ],
    }
    return canonical_json_bytes(
        {
            "target": target,
            "platform": config["platform"],
            "nand": config["nand"],
            "device_data": {"parser": declarations["parser"], "groups": groups},
        }
    )


def _readme(config: dict[str, Any], *, host: str) -> bytes:
    name = target_display_name(config["identity"])
    instructions = config["adapter"]["boot_instructions"]
    if host == "windows10":
        host_instructions = (
            "Use Windows 10 or 11 x64. Python and the host programs are bundled.\n"
            "On first use, Run can ask to install the phone's USB driver.\n"
            "Read the included driver license and approve the administrator prompt.\n"
            "Double-click Run.cmd to start or reconnect.\n"
        )
        dump_command = 'Run.cmd --from-dump "C:\\path\\to\\nand.bin"'
    else:
        host_instructions = (
            "Use Linux x86-64 with coreutils, iproute2, udev and a network manager\n"
            "that automatically runs IPv4 DHCP on new USB interfaces, such as\n"
            "NetworkManager.\n"
            "Python and OpenSSH are bundled. Run asks for administrator access\n"
            "once if the included phone USB access rules are not installed.\n"
            "Open a terminal in this directory and run:\n"
            "  sh Run.sh\n"
        )
        dump_command = "sh Run.sh --from-dump /path/to/nand.bin"
    text = (
        f"FPLinux - {name}\n\n"
        f"This archive is only for {name}. Internal storage is read only.\n\n"
        f"{host_instructions}\n"
        "For a fresh load, keep the phone disconnected until Run displays\n"
        "the connection instructions. Leave a running phone connected to reopen SSH.\n"
        f"{instructions}\n\n"
        "The first run saves a complete backup and prepares your phone's own data.\n"
        "When it finishes, disconnect USB. If the phone stays on, briefly remove\n"
        "and reinsert its battery. Run the same script with the phone powered off\n"
        "to start FPLinux and open SSH. Type exit or press Ctrl+D to close SSH.\n"
        "Open the same launcher again to reconnect while the phone remains on.\n"
        "Only one launcher can run from this directory at a time.\n"
        "Ctrl+C cancels loading or waiting; it does not power the phone off.\n\n"
        "To prepare from an existing complete NAND backup without connecting the phone:\n"
        f"  {dump_command}\n"
        "This preparation preserves the backup and cannot replace existing data/phone.\n\n"
        "Keep this directory for this one phone. Your NAND backup is data/nand.bin,\n"
        "prepared phone data is data/phone, and private connection keys are data/session.\n"
        "Each launch writes a new log in logs and displays its exact path.\n"
    )
    return text.encode("utf-8")


def _linux_host_tool(target: str, platform: str, tool: str, *, offline: bool) -> bytes:
    from fplinux_cli.cli.build import prepare_host_tool  # noqa: PLC0415
    from fplinux_cli.reporting.run import RunReporter  # noqa: PLC0415

    cache = common.ROOT / ".cache"
    cache.mkdir(exist_ok=True)
    reporter = RunReporter.create("package", target=target, verbose=False)
    with tempfile.TemporaryDirectory(dir=cache, prefix=".autonomous-host-") as temporary:
        output = Path(temporary)
        prepare_host_tool(
            target,
            platform,
            tool,
            output,
            offline=offline,
            reporter=reporter,
        )
        executable = output / tool
        if executable.is_symlink() or not executable.is_file():
            fail(f"the prepared host tool is missing or invalid: {tool}")
        contents = executable.read_bytes()
    reporter.finish()
    return contents


def _linux_ecc_sources(platform: str, *, offline: bool) -> dict[str, bytes]:
    """Deliver the verified source projection and license for the NAND corrector."""
    from fplinux_cli.build.host import (  # noqa: PLC0415
        copy_host_project_files,
        extract_host_members,
    )
    from fplinux_cli.build.sources import source_lock_entry  # noqa: PLC0415
    from fplinux_cli.environment.downloads import download_locked_file  # noqa: PLC0415
    from fplinux_cli.manifests.platforms import load_platform  # noqa: PLC0415

    recipe = next(
        item for item in load_platform(platform)["host"]["tools"] if item["name"] == NAND_ECC_TOOL
    )
    sources = common.load_toml(common.ROOT / "sources.lock.toml")
    lock = source_lock_entry(sources, recipe["source_lock"])
    cache = common.ROOT / ".cache"
    archive = download_locked_file(
        lock["archive_url"],
        lock["archive_sha256"],
        cache / "downloads" / recipe["cache_name"],
        offline=offline,
    )
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
    target: str,
    config: dict[str, Any],
    *,
    host: str,
    offline: bool,
) -> AutonomousPayload:
    """Collect runtime code and prepared native dependencies without personal data."""
    files = {
        "Run.py": _source_bytes("common/autonomous.py"),
        "device-data-manifest.json": device_data_manifest(target, config),
        "README.txt": _readme(config, host=host),
    }
    for relative in runtime_python_files(config):
        files[relative] = _source_bytes(relative)
    parser = config["device_data"]["parser"]
    platform = config["platform"]
    nand = config["nand"]
    has_ecc = nand is not None and "ecc_format" in nand
    for relative in (
        f"targets/{target}/{parser}",
        f"platforms/{platform}/host/stock_image.py",
    ):
        files[relative] = _source_bytes(relative)
    if has_ecc:
        relative = f"platforms/{platform}/{NAND_ECC_MODULE}"
        files[relative] = _source_bytes(relative)

    executables = {"Run.py"}
    if host == "windows10":
        from fplinux_cli.build.windows import prepare_windows_host  # noqa: PLC0415
        from fplinux_cli.build.windows_driver import prepare_windows_driver  # noqa: PLC0415

        files["Run.cmd"] = _source_bytes("common/Run.cmd")
        files["common/__init__.py"] = _source_bytes("common/__init__.py")
        files["common/windows_host.py"] = _source_bytes("common/windows_host.py")
        for source in sorted((common.ROOT / "common/windows").rglob("*.ps1")):
            relative = source.relative_to(common.ROOT).as_posix()
            files[relative] = _source_bytes(relative)
        dependencies = prepare_windows_host(
            cache=common.ROOT / ".cache", offline=offline, nand_ecc=has_ecc
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
        from fplinux_cli.build.linux_host import prepare_linux_host  # noqa: PLC0415

        files["Run.sh"] = _source_bytes("common/Run.sh")
        files["tools/fphelper_t117"] = _linux_host_tool(
            target, platform, "fphelper_t117", offline=offline
        )
        files["licenses/host-tools/Unlicense.txt"] = _source_bytes("LICENSES/Unlicense.txt")
        executables.update({"Run.sh", "tools/fphelper_t117"})
        if has_ecc:
            relative = f"tools/{NAND_ECC_TOOL}"
            files[relative] = _linux_host_tool(target, platform, NAND_ECC_TOOL, offline=offline)
            executables.add(relative)
            files.update(_linux_ecc_sources(platform, offline=offline))
        for relative, source in prepare_linux_host(
            cache=common.ROOT / ".cache", offline=offline
        ).items():
            if relative in files:
                fail(f"duplicate Linux archive path: {relative}")
            files[relative] = source.read_bytes()
            if source.stat().st_mode & 0o111:
                executables.add(relative)
    return AutonomousPayload(files, executables)

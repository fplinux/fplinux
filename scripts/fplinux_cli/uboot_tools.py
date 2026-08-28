# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- validation failures use exact artifact diagnostics.
"""Build and receipt the profile-selected U-Boot FIT tools."""

from __future__ import annotations

import hashlib
import shutil
import struct
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from . import artifact_state, build_env
from .artifact_state import (
    canonical_json_bytes,
    receipt_matches,
    regular_file_record,
    require_lowercase_sha256,
    write_canonical_json,
)
from .common import sha256_file

RECEIPT_NAME = ".fplinux-uboot-receipt.json"
_BUILD_TIMEOUT_SECONDS = 1800
_TOOL_TIMEOUT_SECONDS = 30


class UbootToolsError(RuntimeError):
    """A U-Boot tools source, build or receipt is invalid."""


@dataclass(frozen=True)
class UbootBuild:
    """Verified full U-Boot second stage and its matching FIT tools."""

    mkimage: Path
    dumpimage: Path
    elf: Path
    binary: Path
    dtb: Path
    map: Path
    config: Path
    entry: int
    receipt: dict[str, str]


def _tool_version(path: Path, version: str) -> None:
    result = subprocess.run(
        [str(path), "-V"],
        capture_output=True,
        text=True,
        env=build_env.build_environment(),
        check=False,
        timeout=_TOOL_TIMEOUT_SECONDS,
    )
    output = "\n".join(part.strip() for part in (result.stdout, result.stderr) if part.strip())
    expected = f"{path.name} version {version}"
    if result.returncode != 0 or output != expected:
        raise UbootToolsError(
            f"unexpected {path.name} self-test result: {output or f'exit {result.returncode}'}"
        )


def _extract_archive(archive: Path, destination: Path, prefix: str) -> Path:
    expected_root = PurePosixPath(prefix)
    if expected_root.is_absolute() or ".." in expected_root.parts:
        raise UbootToolsError("U-Boot archive prefix is invalid")
    unpacked = destination / ".u-boot-source.tar"
    try:
        with unpacked.open("wb") as stream:
            subprocess.run(
                ["bzip2", "-dc", str(archive)],
                stdout=stream,
                check=True,
                timeout=_BUILD_TIMEOUT_SECONDS,
            )
        with tarfile.open(unpacked, "r:") as source:
            members = source.getmembers()
            if not members:
                raise UbootToolsError("U-Boot archive is empty")
            for member in members:
                path = PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise UbootToolsError(f"unsafe U-Boot archive member: {member.name}")
                if path.parts[: len(expected_root.parts)] != expected_root.parts:
                    raise UbootToolsError(f"unexpected U-Boot archive member: {member.name}")
            source.extractall(destination, filter="data")
    finally:
        unpacked.unlink(missing_ok=True)
    extracted = destination / prefix
    if extracted.is_symlink() or not extracted.is_dir():
        raise UbootToolsError("U-Boot archive did not contain its declared source root")
    return extracted


def _run(command: list[str], *, cwd: Path | None = None) -> None:
    subprocess.run(
        command,
        cwd=cwd,
        env=build_env.build_environment(),
        check=True,
        timeout=_BUILD_TIMEOUT_SECONDS,
    )


FULL_OUTPUTS = (
    "u-boot",
    "u-boot.bin",
    "u-boot.dtb",
    "u-boot.map",
    ".config",
    "mkimage",
    "dumpimage",
)


def _full_recipe(  # noqa: PLR0913, PLR0917 -- causal inputs stay explicit.
    archive: Path,
    config: dict[str, Any],
    defconfig: Path,
    projections: list[tuple[Path, str]],
    patches: list[Path],
    container_recipe: str,
    cross_compile: str,
) -> str:
    lock = config["lock"]
    archive_digest = require_lowercase_sha256(
        lock.get("archive_sha256"), "U-Boot archive", UbootToolsError
    )
    if sha256_file(archive) != archive_digest:
        raise UbootToolsError("downloaded U-Boot archive differs from its source lock")
    manifest = {
        "source": {
            "version": lock.get("version"),
            "archive_sha256": archive_digest,
            "archive_prefix": config.get("archive_prefix"),
        },
        "defconfig": regular_file_record(defconfig, "U-Boot tools file", UbootToolsError),
        "projections": [
            {
                "destination": destination,
                "source": regular_file_record(source, "U-Boot tools file", UbootToolsError),
            }
            for source, destination in projections
        ],
        "patches": [
            regular_file_record(path, "U-Boot tools file", UbootToolsError) for path in patches
        ],
        "container_image_recipe": require_lowercase_sha256(
            container_recipe, "container image recipe", UbootToolsError
        ),
        "cross_compile": cross_compile,
        "source_date_epoch": build_env.SOURCE_DATE_EPOCH,
        "implementation": {
            "uboot_tools": regular_file_record(
                Path(__file__), "U-Boot tools file", UbootToolsError
            ),
            "artifact_state": regular_file_record(
                Path(artifact_state.__file__), "U-Boot tools file", UbootToolsError
            ),
            "build_env": regular_file_record(
                Path(build_env.__file__), "U-Boot tools file", UbootToolsError
            ),
        },
    }
    return hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()


def _full_receipt(recipe: str, output: Path) -> dict[str, object]:
    return {
        "recipe": recipe,
        "outputs": {
            name: regular_file_record(output / name, "U-Boot tools file", UbootToolsError)
            for name in FULL_OUTPUTS
        },
    }


def _full_cache_hit(recipe: str, output: Path) -> bool:
    try:
        return receipt_matches(output / RECEIPT_NAME, _full_receipt(recipe, output))
    except UbootToolsError:
        return False


def _copy_projection(source: Path, root: Path, destination: str) -> None:
    target = root / destination
    if target.exists() or target.is_symlink():
        raise UbootToolsError(f"U-Boot projection destination already exists: {destination}")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def _config_integer(config: str, symbol: str) -> int:
    """Read one unique integer assignment from an actual U-Boot config."""
    prefix = f"{symbol}="
    values = [line.removeprefix(prefix) for line in config.splitlines() if line.startswith(prefix)]
    if len(values) != 1:
        raise UbootToolsError(f"full U-Boot config lacks one {symbol} assignment")
    try:
        return int(values[0], 0)
    except ValueError as error:
        raise UbootToolsError(f"full U-Boot config has an invalid {symbol} value") from error


def _verify_full_output(output: Path, layout: dict[str, int]) -> int:
    elf = output / "u-boot"
    regular_file_record(elf, "U-Boot tools file", UbootToolsError)
    with elf.open("rb") as stream:
        header = stream.read(52)
    entry = struct.unpack_from("<I", header, 24)[0] if len(header) == 52 else 0
    if (
        len(header) != 52
        or header[:4] != b"\x7fELF"
        or header[4] != 1
        or header[5] != 1
        or struct.unpack_from("<H", header, 18)[0] != 40
    ):
        raise UbootToolsError("full U-Boot ELF identity is invalid")
    binary = output / "u-boot.bin"
    binary_size = binary.stat().st_size
    load = layout["uboot_load"]
    limit = layout["uboot_size"]
    if binary_size <= 0 or binary_size > limit:
        raise UbootToolsError("full U-Boot binary exceeds its profile load arena")
    if entry < load or entry - load >= binary_size:
        raise UbootToolsError("full U-Boot ELF entry lies outside its loaded binary")
    config = (output / ".config").read_text(encoding="utf-8")
    required = (
        "CONFIG_TARGET_FPLINUX_TA1618=y\n",
        "CONFIG_ENV_IS_NOWHERE=y\n",
        "CONFIG_AUTOBOOT=y\n",
        "CONFIG_BOOTDELAY=-2\n",
        "CONFIG_USE_BOOTCOMMAND=y\n",
        'CONFIG_BOOTCOMMAND="sdboot"\n',
        "# CONFIG_BOOTSTD is not set\n",
        "CONFIG_SYS_DCACHE_OFF=y\n",
        "CONFIG_FIT=y\n",
        "CONFIG_FIT_FULL_CHECK=y\n",
        "CONFIG_SHA256=y\n",
        "CONFIG_LMB=y\n",
        "# CONFIG_FIT_SIGNATURE is not set\n",
        "# CONFIG_LEGACY_IMAGE_FORMAT is not set\n",
        "CONFIG_CMD_BOOTM=y\n",
        "CONFIG_MMC=y\n",
        "CONFIG_DM_MMC=y\n",
        "# CONFIG_CMD_MMC is not set\n",
        "# CONFIG_MMC_WRITE is not set\n",
        "# CONFIG_MMC_HW_PARTITIONING is not set\n",
        "# CONFIG_CMD_FAT is not set\n",
        "# CONFIG_CMD_FS_GENERIC is not set\n",
        "CONFIG_FS_FAT=y\n",
        "CONFIG_DOS_PARTITION=y\n",
        "# CONFIG_FAT_WRITE is not set\n",
        "# CONFIG_BLOCK_CACHE is not set\n",
        "# CONFIG_USB is not set\n",
        "# CONFIG_NET is not set\n",
        "# CONFIG_EFI_LOADER is not set\n",
    )
    forbidden = (
        "CONFIG_FAT_WRITE=y\n",
        "CONFIG_ENV_IS_IN_MMC=y\n",
        "CONFIG_ENV_IS_IN_FAT=y\n",
        "CONFIG_ENV_IS_IN_EXT4=y\n",
        "CONFIG_SUPPORT_EMMC_BOOT=y\n",
        "CONFIG_CMD_MMC_RPMB=y\n",
        "CONFIG_CMD_MMC_SWRITE=y\n",
        "CONFIG_CMD_BKOPS_ENABLE=y\n",
    )
    missing = [value.rstrip() for value in required if value not in config]
    enabled = [value.rstrip() for value in forbidden if value in config]
    if missing or enabled:
        detail = "; ".join(
            part
            for part in (
                "missing: " + ", ".join(missing) if missing else "",
                "forbidden: " + ", ".join(enabled) if enabled else "",
            )
            if part
        )
        message = f"full U-Boot config violates the read-only MMC contract: {detail}"
        raise UbootToolsError(message)
    if _config_integer(config, "CONFIG_TEXT_BASE") != load:
        raise UbootToolsError("full U-Boot text base differs from the profile layout")
    if _config_integer(config, "CONFIG_CUSTOM_SYS_INIT_SP_ADDR") != layout["uboot_stack"]:
        raise UbootToolsError("full U-Boot stack differs from the profile layout")
    if _config_integer(config, "CONFIG_SYS_LOAD_ADDR") != layout["fit_load"]:
        raise UbootToolsError("full U-Boot default load address differs from the FIT arena")
    if _config_integer(config, "CONFIG_SYS_FDT_PAD") != layout["fdt_pad"]:
        raise UbootToolsError("full U-Boot FDT padding differs from the profile layout")
    if elf.stat().st_size <= binary.stat().st_size:
        raise UbootToolsError("full U-Boot ELF does not contain expected debug metadata")
    return entry


def _full_result(output: Path, recipe: str, layout: dict[str, int]) -> UbootBuild:
    entry = _verify_full_output(output, layout)
    return UbootBuild(
        output / "mkimage",
        output / "dumpimage",
        output / "u-boot",
        output / "u-boot.bin",
        output / "u-boot.dtb",
        output / "u-boot.map",
        output / ".config",
        entry,
        {"recipe": recipe, "sha256": sha256_file(output / RECEIPT_NAME)},
    )


def build_full(  # noqa: PLR0913, PLR0917 -- build inputs stay explicit.
    archive: Path,
    config: dict[str, Any],
    defconfig: Path,
    projections: list[tuple[Path, str]],
    patches: list[Path],
    work: Path,
    jobs: int,
    container_recipe: str,
    cross_compile: str,
    layout: dict[str, int],
) -> UbootBuild:
    """Build or exactly reuse the read-only MMC TA-1618 full U-Boot."""
    if jobs < 1:
        raise UbootToolsError("full U-Boot jobs must be positive")
    recipe = _full_recipe(
        archive,
        config,
        defconfig,
        projections,
        patches,
        container_recipe,
        cross_compile,
    )
    version = str(config["lock"]["version"])
    output = work / "uboot"
    if _full_cache_hit(recipe, output):
        _verify_full_output(output, layout)
        return _full_result(output, recipe, layout)

    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work, prefix=".uboot.") as temporary_name:
        temporary = Path(temporary_name)
        source = _extract_archive(archive, temporary, str(config["archive_prefix"]))
        for projection_source, destination in projections:
            _copy_projection(projection_source, source, destination)
        for patch in patches:
            _run(
                ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(patch)],
                cwd=source,
            )
        if not defconfig.name.endswith("_defconfig"):
            raise UbootToolsError("selected U-Boot defconfig name is invalid")
        defconfig_target = source / "configs" / defconfig.name
        if defconfig_target.exists() or defconfig_target.is_symlink():
            raise UbootToolsError("U-Boot defconfig destination already exists")
        shutil.copyfile(defconfig, defconfig_target)
        build = temporary / "build"
        make = ["make", "-C", str(source), f"O={build}", f"CROSS_COMPILE={cross_compile}"]
        _run([*make, defconfig.name])
        _run([*make, f"-j{jobs}", "all"])

        staging = temporary / "publish"
        staging.mkdir()
        built_outputs = {
            "u-boot": build / "u-boot",
            "u-boot.bin": build / "u-boot-dtb.bin",
            "u-boot.dtb": build / "u-boot.dtb",
            "u-boot.map": build / "u-boot.map",
            ".config": build / ".config",
            "mkimage": build / "tools/mkimage",
            "dumpimage": build / "tools/dumpimage",
        }
        for name, built in built_outputs.items():
            if built.is_symlink() or not built.is_file():
                raise UbootToolsError(f"full U-Boot build did not produce {built}")
            shutil.copyfile(built, staging / name)
            (staging / name).chmod(0o755 if name in {"mkimage", "dumpimage"} else 0o644)
        _tool_version(staging / "mkimage", version)
        _tool_version(staging / "dumpimage", version)
        _verify_full_output(staging, layout)
        write_canonical_json(staging / RECEIPT_NAME, _full_receipt(recipe, staging))
        if output.exists():
            if output.is_symlink() or not output.is_dir():
                raise UbootToolsError(f"full U-Boot output is invalid: {output}")
            shutil.rmtree(output)
        staging.replace(output)

    if not _full_cache_hit(recipe, output):
        raise UbootToolsError("published full U-Boot receipt is not reusable")
    _verify_full_output(output, layout)
    return _full_result(output, recipe, layout)

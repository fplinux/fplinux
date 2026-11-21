# SPDX-License-Identifier: GPL-2.0-only
"""Produce the prebuilt microSD components installed by the RAM reader."""

from __future__ import annotations

import os
import shutil
import stat
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli import common
from fplinux_cli.alpine.rootfs_files import (
    _normalize_rootfs,
    extract_rootfs,
    write_card_rootfs_layer,
)
from fplinux_cli.alpine.rootfs_state import trusted_receipt_identity
from fplinux_cli.artifacts.card_kit import (
    CARD_APK_INDEX,
    CARD_APK_KEY,
    CARD_APK_STATE,
    CARD_DEFAULT_APPS,
    CardKitError,
    locate_fit_ramdisk,
)
from fplinux_cli.artifacts.records import write_canonical_json
from fplinux_cli.artifacts.release import current_release_context
from fplinux_cli.build import process as process_build
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.build.inputs import require_file
from fplinux_cli.common import fail, sha256_file
from fplinux_cli.manifests.values import sha256_value

from . import ext4, sd

if TYPE_CHECKING:
    from collections.abc import Mapping

    from fplinux_cli.build.bootstrap.result import ColdSdBootstrap


_DATA_DIRECTORIES = "alpine/card-data-directories"
_SLOT_ROOT = Path("system/a/root")
_APK_STATE_FILES = ("etc/apk/arch", "etc/apk/world", "lib/apk/db/installed")
_APK_KEY_DIRECTORIES = ("etc/apk/keys", "lib/apk/keys")


def write_apk_state(rootfs: Path, output: Path) -> None:
    """Project the installed APK state of the composed board root for the solver."""
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".apk-state.") as name:
        work = Path(name)
        key_patterns = tuple(f"{directory}/*.pub" for directory in _APK_KEY_DIRECTORIES)
        process_build.run(
            [
                "cpio",
                "--extract",
                "--quiet",
                "--make-directories",
                "--no-preserve-owner",
                "--file",
                str(rootfs),
                "--directory",
                str(work),
                *_APK_STATE_FILES,
                *key_patterns,
            ]
        )
        files = [work / path for path in _APK_STATE_FILES]
        for directory in _APK_KEY_DIRECTORIES:
            files.extend((work / directory).glob("*.pub"))

        archive = work / output.name
        with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as payload:
            for path in sorted(files):
                require_file(path)
                metadata = path.stat()
                member = tarfile.TarInfo(path.relative_to(work).as_posix())
                member.mode = stat.S_IMODE(metadata.st_mode)
                member.mtime = int(SOURCE_DATE_EPOCH)
                member.size = metadata.st_size
                with path.open("rb") as source:
                    payload.addfile(member, source)
        archive.chmod(0o644)
        archive.replace(output)


def _card_directories(root: Path, path: Path) -> None:
    for component in (*reversed(path.parents), path):
        directory = root / component
        if directory.is_symlink() or (directory.exists() and not directory.is_dir()):
            fail(f"system card directory is invalid: {component}")
        if not directory.exists():
            directory.mkdir()
            directory.chmod(0o755)


def _stage_card_root(archive: Path, partition: Path, directories: Path) -> None:
    """Stage one writable release tree and its shared mount points."""
    slot = partition / _SLOT_ROOT
    _card_directories(partition, _SLOT_ROOT.parent)
    extract_rootfs(archive, slot)
    for line in directories.read_text(encoding="ascii").splitlines():
        relative_name, mode = line.split()
        relative = Path(relative_name)
        _card_directories(slot, relative)
        _card_directories(partition, Path("data") / relative.parent)
        persistent = partition / "data" / relative
        (slot / relative).rename(persistent)
        persistent.chmod(int(mode, 8))
        (slot / relative).mkdir()
        (slot / relative).chmod(int(mode, 8))


def build_partition(  # noqa: PLR0913 -- keep the fit and normalized root as separate inputs.
    target: str,
    spec: dict[str, Any],
    rootfs: Path,
    rootfs_recipe: str,
    output: Path,
    *,
    fit: Path | None,
) -> Path:
    """Build an ext4 partition for a factory card or the installer kit."""
    directories = common.ROOT / _DATA_DIRECTORIES
    selected = {
        **spec,
        "card_layout": {
            "implementation": sha256_file(Path(__file__)),
            "data_directories": sha256_file(directories),
            "factory": {"target": target, "fit": sha256_file(fit)} if fit is not None else None,
        },
    }
    expected_files: tuple[str, ...] = (str(_SLOT_ROOT / "etc/os-release"),)
    if fit is not None:
        expected_files += (
            "system/active",
            "system/target",
            "system/a/FPLINUX.ITB",
            "system/a/base-world",
        )
    plan = ext4.create_plan(
        selected,
        rootfs_recipe,
        trusted_receipt_identity(rootfs.parent, rootfs_recipe),
        sha256_value(os.environ.get("FPLINUX_ENVIRONMENT_ARTIFACT_RECIPE"), "environment recipe"),
        expected_files=expected_files,
    )
    if ext4.cache_hit(output, plan):
        return output / str(spec["filename"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".card-tree.") as name:
        partition = Path(name) / "partition"
        partition.mkdir(mode=0o755)
        _stage_card_root(rootfs, partition, directories)
        if fit is not None:
            system = partition / "system"
            shutil.copyfile(fit, system / "a/FPLINUX.ITB")
            (system / "a/FPLINUX.ITB").chmod(0o644)
            world = system / "a/base-world"
            shutil.copyfile(partition / _SLOT_ROOT / "etc/fplinux/base-world", world)
            world.chmod(0o644)
            for filename, value in (("active", "a"), ("target", target)):
                record = system / filename
                record.write_text(f"{value}\n", encoding="ascii")
                record.chmod(0o600)
        _normalize_rootfs(partition)
        return ext4.build(partition, output, plan)


def build(  # noqa: PLR0913 -- each selected source and artifact stay explicit.
    target: str,
    target_config: dict[str, Any],
    output: Path,
    *,
    board_rootfs: Path,
    shared_rootfs: Path,
    shared_ext4: Path | None,
    fit: Path | None,
    ramdisk: Path | None,
    cold_boot: ColdSdBootstrap | None,
    bundle_apks: Mapping[str, Path],
    apk_catalogue: Mapping[str, Path],
    applications: tuple[str, ...],
) -> dict[str, Path]:
    """Produce boot, system and APK files for the installed system card."""
    if shared_ext4 is None or fit is None or ramdisk is None:
        fail("release card requires a verified ext4 root and FIT")
    if cold_boot is None:
        fail("release card requires a verified autonomous bootstrap")
    if set(apk_catalogue) != {CARD_APK_INDEX, CARD_APK_KEY}:
        fail("release card requires the signed APK catalogue")
    output.mkdir(parents=True, exist_ok=True)
    storage = target_config["storage"]
    prefix = output / "boot.xz"
    root = output / "root.ext4.xz"
    board = output / "board.tar"
    layout = output / "layout.json"
    apk_state = output / CARD_APK_STATE
    default_apps = output / CARD_DEFAULT_APPS

    try:
        ramdisk_payload = ramdisk.read_bytes()
        ramdisk_offset, hash_offset = locate_fit_ramdisk(fit.read_bytes(), ramdisk_payload)
        prefix_digest = sd.write_boot_prefix(
            shared_ext4, prefix, storage=storage, cold_boot=cold_boot
        )
        root_digest = sd.compress_card_region(shared_ext4, root, storage["root_size"])
        write_card_rootfs_layer(board_rootfs, shared_rootfs, board)
        write_apk_state(board_rootfs, apk_state)
        default_apps.write_text(
            "".join(f"{name}\n" for name in applications), encoding="ascii", newline="\n"
        )
        default_apps.chmod(0o644)
        write_canonical_json(
            layout,
            {
                "target": target,
                "release_id": current_release_context().release_id,
                "boot_offset": storage["boot_offset"],
                "boot_size": storage["boot_size"],
                "root_offset": storage["root_offset"],
                "root_size": storage["root_size"],
                "disk_signature": storage["disk_signature"],
                "boot_sha256": prefix_digest,
                "root_sha256": root_digest,
                "fit_filename": target_config["fit"]["filename"],
                "fit_ramdisk_offset": ramdisk_offset,
                "fit_ramdisk_size": len(ramdisk_payload),
                "fit_hash_offset": hash_offset,
            },
        )
    except (OSError, CardKitError, sd.SdImageError) as error:
        fail(str(error))

    return {
        "card/boot.xz": prefix,
        "card/root.ext4.xz": root,
        "card/board.tar": board,
        "card/fit.itb": fit,
        "card/layout.json": layout,
        f"card/{CARD_APK_STATE}": apk_state,
        f"card/{CARD_DEFAULT_APPS}": default_apps,
        **{f"card/{name}": path for name, path in apk_catalogue.items()},
        **{f"card/apks/{name}.apk": path for name, path in bundle_apks.items()},
    }

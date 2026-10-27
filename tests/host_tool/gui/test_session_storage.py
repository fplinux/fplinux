# SPDX-License-Identifier: GPL-2.0-only
"""Storage policy over real Linux-shaped fixtures and replaced mount syscalls."""

from __future__ import annotations

import errno
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path


@dataclass(frozen=True)
class Volume:
    """One observed volume returned by the storage peer."""

    identifier: int
    state: int
    protection: int
    read_only: bool
    label: str
    path: str
    mount_id: int


@dataclass(frozen=True)
class Result:
    """Outcome and volume snapshot of one storage request."""

    error: int
    persistent: bool
    volumes: list[Volume]


@pytest.fixture(scope="module")
def storage_peer(tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
    with c_build_directory(tmp_path_factory, "session-storage") as build:
        executable = build / "storage"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                f"-I{ROOT / 'include/fplinux'}",
                f"-I{ROOT / 'alpine/aports/fplinux-session'}",
                str(ROOT / "tests/host_tool/gui/storage/session-storage.c"),
                str(ROOT / "alpine/aports/fplinux-session/session-storage.c"),
                str(ROOT / "lib/fplinux/fplinux-utf8.c"),
            ],
            name="compile session storage component peer",
        )
        yield executable


@pytest.fixture
def storage_tree(tmp_path: Path) -> Path:
    (tmp_path / "sys").mkdir()
    (tmp_path / "dev").mkdir()
    (tmp_path / "mountinfo").write_text("1 0 0:1 / / rw - tmpfs tmpfs rw\n")
    (tmp_path / "swaps").write_text("Filename\tType\tSize\tUsed\tPriority\n")
    return tmp_path


# These independent Linux attributes select distinct discovery and safety paths.
def add_card(  # noqa: PLR0913
    tree: Path,
    *,
    disk: str = "mmcblk0",
    kind: str = "SD",
    filesystem: str = "vfat",
    partition: int | None = 1,
    label: str = "Documents",
    read_only: bool = False,
) -> str:
    device = tree / "sys" / disk / "device"
    device.mkdir(parents=True)
    (device / "type").write_text(f"{kind}\n")
    (device / "cid").write_text(f"card-{disk}\n")
    (device.parent / "dev").write_text("179:0\n")
    (device.parent / "ro").write_text(f"{int(read_only)}\n")
    # Current phone slots may have removable=0 despite accepting SD cards.
    (device.parent / "removable").write_text("0\n")
    selected = disk
    if partition is not None:
        selected = f"{disk}p{partition}"
        entry = tree / "sys" / selected
        entry.mkdir()
        (entry / "dev").write_text(f"179:{partition}\n")
        (entry / "partition").write_text(f"{partition}\n")
        (entry / "ro").write_text(f"{int(read_only)}\n")
    (tree / "dev" / f"{selected}.probe").write_text(f"{filesystem}\n{label}\n")
    return selected


def invoke(peer: Path, tree: Path, *actions: str) -> list[Result]:
    process = run_process(
        [str(peer), str(tree), *actions],
        name="exercise storage detection and managed volume transitions",
        timeout=15,
        check=True,
    )
    results: list[Result] = []
    for line in process.stdout.splitlines():
        fields = line.split("\t")
        if fields[0] == "result":
            results.append(Result(int(fields[2]), bool(int(fields[4])), []))
        else:
            assert fields[0] == "volume"
            results[-1].volumes.append(
                Volume(
                    int(fields[1]),
                    int(fields[2]),
                    int(fields[3]),
                    bool(int(fields[4])),
                    fields[5],
                    fields[6],
                    int(fields[7]),
                )
            )
    return results


def effects(tree: Path) -> list[str]:
    path = tree / "effects"
    return path.read_text().splitlines() if path.exists() else []


def test_no_card_is_normal_and_does_not_create_mount_directory(
    storage_peer: Path, storage_tree: Path
) -> None:
    result = invoke(storage_peer, storage_tree, "refresh")[0]
    assert result == Result(error=0, persistent=False, volumes=[])
    assert not (storage_tree / "card data").exists()
    assert effects(storage_tree) == []


def test_internal_mmc_is_not_a_user_volume(storage_peer: Path, storage_tree: Path) -> None:
    add_card(storage_tree, kind="MMC")
    assert invoke(storage_peer, storage_tree, "refresh")[0].volumes == []


def test_non_utf8_card_label_uses_safe_display_name(
    storage_peer: Path, storage_tree: Path
) -> None:
    selected = add_card(storage_tree)
    (storage_tree / "dev" / f"{selected}.probe").write_bytes(b"vfat\nOEM \xff label\n")
    result = invoke(storage_peer, storage_tree, "refresh")[0]
    assert result.error == 0
    assert result.volumes[0].state == 0
    assert result.volumes[0].label == "microSD"


@pytest.mark.parametrize("filesystem", ["vfat", "ext4"])
@pytest.mark.parametrize("partition", [None, 1])
def test_supported_card_connects_with_app_access_and_preserves_ext4_permissions(
    storage_peer: Path, storage_tree: Path, filesystem: str, partition: int | None
) -> None:
    selected = add_card(storage_tree, filesystem=filesystem, partition=partition)
    initial, repeated, connected = invoke(
        storage_peer, storage_tree, "refresh", "refresh", "connect"
    )
    assert initial.error == repeated.error == connected.error == 0
    assert initial.volumes[0].state == 0
    assert initial.volumes[0].mount_id == 0
    assert initial.volumes[0].identifier == repeated.volumes[0].identifier
    assert connected.volumes[0].identifier != initial.volumes[0].identifier
    assert connected.volumes[0].state == 1
    assert connected.volumes[0].mount_id == 31
    assert connected.volumes[0].path == str(storage_tree / "card data")
    assert connected.volumes[0].label == "Documents"
    options = "uid=1000,gid=1000,dmask=0022,fmask=0133,utf8=1" if filesystem == "vfat" else ""
    assert effects(storage_tree) == [
        f"mount {storage_tree / 'dev' / selected} {filesystem} rw {options}"
    ]


def test_read_only_card_remains_read_only_after_connect(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree, read_only=True)
    _, connected = invoke(storage_peer, storage_tree, "refresh", "connect")
    assert connected.error == 0
    assert connected.volumes[0].read_only
    assert " vfat ro " in effects(storage_tree)[0]


@pytest.mark.parametrize(
    ("filesystem", "partition"),
    [("exfat", 1), ("vfat", 2)],
)
def test_unsupported_filesystem_or_missing_first_partition_cannot_connect(
    storage_peer: Path, storage_tree: Path, filesystem: str, partition: int
) -> None:
    add_card(storage_tree, filesystem=filesystem, partition=partition)
    initial, connected = invoke(storage_peer, storage_tree, "refresh", "connect")
    assert initial.volumes[0].state == 2
    assert connected.error == errno.EOPNOTSUPP
    assert effects(storage_tree) == []


@pytest.mark.parametrize(("protection", "expected"), [("root", 1), ("swap", 2), ("boot", 3)])
def test_card_used_by_root_swap_or_boot_is_protected_as_a_physical_medium(
    storage_peer: Path, storage_tree: Path, protection: str, expected: int
) -> None:
    add_card(storage_tree, label="FPLBOOT" if protection == "boot" else "Documents")
    other = storage_tree / "sys/mmcblk0p2"
    other.mkdir()
    (other / "dev").write_text("179:2\n")
    if protection == "root":
        (storage_tree / "mountinfo").write_text("1 0 179:2 / / rw - ext4 /dev/root rw\n")
    elif protection == "swap":
        (storage_tree / "swaps").write_text(
            "Filename\tType\tSize\tUsed\tPriority\n"
            f"{storage_tree}/dev/mmcblk0p2 partition 4096 0 -2\n"
        )
    initial, connect, eject = invoke(storage_peer, storage_tree, "refresh", "connect", "eject")
    assert initial.volumes[0].state == 3
    assert initial.volumes[0].protection == expected
    assert initial.volumes[0].path == ""
    assert connect.error == eject.error == errno.EPERM
    assert effects(storage_tree) == []


def test_busy_eject_preserves_mount_then_normal_eject_removes_path(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    _, mounted, busy, _, ejected = invoke(
        storage_peer, storage_tree, "refresh", "connect", "busy-eject", "unbusy", "eject"
    )
    assert mounted.volumes[0].state == 1
    assert busy.error == errno.EBUSY
    assert busy.volumes[0].state == 1
    assert busy.volumes[0].identifier == mounted.volumes[0].identifier
    assert ejected.error == 0
    assert ejected.volumes[0].state == 0
    assert ejected.volumes[0].path == ""
    assert ejected.volumes[0].mount_id == 0
    assert ejected.volumes[0].identifier != mounted.volumes[0].identifier
    assert effects(storage_tree)[1:] == ["sync", "unmount", "sync", "unmount"]


def test_failed_sync_keeps_card_mounted_without_unmount(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    (storage_tree / "sync-error").write_text(f"{errno.EIO}\n")
    _, _, ejected = invoke(storage_peer, storage_tree, "refresh", "connect", "eject")
    assert ejected.error == errno.EIO
    assert ejected.volumes[0].state == 1
    assert effects(storage_tree)[1:] == ["sync"]


def test_other_mounted_partition_blocks_connect_and_eject(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    other = storage_tree / "sys/mmcblk0p2"
    other.mkdir()
    (other / "dev").write_text("179:2\n")
    (storage_tree / "mountinfo").write_text(
        "1 0 0:1 / / rw - tmpfs tmpfs rw\n2 1 179:2 / /other rw - ext4 /dev/mmcblk0p2 rw\n"
    )
    initial, connect, eject = invoke(storage_peer, storage_tree, "refresh", "connect", "eject")
    assert initial.volumes[0].state == 3
    assert initial.volumes[0].protection == 4
    assert initial.volumes[0].path == ""
    assert connect.error == eject.error == errno.EPERM
    assert effects(storage_tree) == []


def test_unrelated_mount_occupying_managed_path_is_not_replaced(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    mountpoint = str(storage_tree / "card data").replace(" ", "\\040")
    (storage_tree / "mountinfo").write_text(
        f"1 0 0:1 / / rw - tmpfs tmpfs rw\n2 1 8:1 / {mountpoint} rw - ext4 /dev/unrelated rw\n"
    )
    _, connected = invoke(storage_peer, storage_tree, "refresh", "connect")
    assert connected.error == errno.EBUSY
    assert connected.volumes[0].path == ""
    assert effects(storage_tree) == []


def test_changed_mount_identity_rejects_eject_with_old_id(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    _, mounted, changed = invoke(storage_peer, storage_tree, "refresh", "connect", "mount-changed")
    assert changed.error == errno.ESTALE
    assert changed.volumes[0].state == 1
    assert changed.volumes[0].identifier != mounted.volumes[0].identifier
    assert mounted.volumes[0].mount_id == 31
    assert changed.volumes[0].mount_id == 32
    assert effects(storage_tree)[1:] == []


def test_unknown_mount_state_does_not_return_the_old_card_path(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    _, mounted, failed = invoke(storage_peer, storage_tree, "refresh", "connect", "scan-error")
    assert mounted.volumes[0].path
    assert failed.error == errno.EIO
    assert failed.volumes == []


def test_mount_id_overflow_does_not_expose_an_unverifiable_path(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    _, _, overflow = invoke(storage_peer, storage_tree, "refresh", "connect", "mount-overflow")
    assert overflow.error == errno.EOVERFLOW
    assert overflow.volumes == []


@pytest.mark.parametrize("action", ["replace", "remove"])
def test_stale_medium_cannot_be_mounted_by_an_old_id(
    storage_peer: Path, storage_tree: Path, action: str
) -> None:
    add_card(storage_tree)
    initial, changed = invoke(storage_peer, storage_tree, "refresh", action)
    assert changed.error == errno.ESTALE
    assert effects(storage_tree) == []
    assert not (storage_tree / "card data").exists()
    if action == "remove":
        assert changed.volumes == []
    else:
        assert changed.volumes[0].identifier != initial.volumes[0].identifier


def test_refresh_runs_outside_caller_and_returns_one_fd_notification(
    storage_peer: Path, storage_tree: Path
) -> None:
    add_card(storage_tree)
    result = invoke(storage_peer, storage_tree, "async")[0]
    assert result.error == 0
    assert result.volumes[0].state == 0


@pytest.mark.parametrize(("filesystem", "expected"), [("tmpfs", False), ("ext4", True)])
def test_home_persistence_uses_the_mount_containing_home(
    storage_peer: Path, storage_tree: Path, filesystem: str, *, expected: bool
) -> None:
    (storage_tree / "mountinfo").write_text(
        "1 0 0:1 / / rw - tmpfs tmpfs rw\n"
        f"2 1 8:1 / {storage_tree}/home rw - {filesystem} /dev/home rw\n"
    )
    assert invoke(storage_peer, storage_tree, "refresh")[0].persistent is expected

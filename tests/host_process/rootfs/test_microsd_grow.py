# SPDX-License-Identifier: GPL-2.0-only
"""Exercise system-card growth and shutdown as processes with controlled kernel and tool peers."""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.process import run_process

if TYPE_CHECKING:
    import subprocess
    from pathlib import Path

GROW = ROOT / "alpine/aports/fplinux-microsd-root/fplinux-microsd-grow"
FIXTURES = ROOT / "tests/host_process/rootfs/fixtures"
SLOTTED_MOUNTS = (
    "39 1 179:10 /system/b/root / rw,relatime - ext4 /dev/mmcblk1p2 rw\n"
    "40 39 0:26 / /run rw,nosuid,nodev - tmpfs tmpfs rw\n"
    "41 40 179:10 / /run/fplinux-card rw,nosuid,nodev - ext4 /dev/mmcblk1p2 rw\n"
    "42 39 179:10 /data/home/fplinux /home/fplinux rw - ext4 /dev/mmcblk1p2 rw\n"
    "43 39 179:2 / /media/other rw - ext4 /dev/mmcblk0p2 rw\n"
)
DIRECT_MOUNTS = (
    "39 1 179:10 / / rw - ext4 /dev/root rw\n40 39 0:26 / /run rw,nosuid,nodev - tmpfs tmpfs rw\n"
)
SWAPS = "Filename Type Size Used Priority\n/dev/zram0 partition 65532 3312 -2\n"
SHARED_MOUNTS = (
    "44 40 179:9 / /run/fplinux-shared-card/fat rw,nosuid,nodev,noexec - vfat /dev/alias rw\n"
    "45 39 179:9 /SHARED /mnt/card rw,nosuid,nodev,noexec - vfat /dev/alias rw\n"
)


@dataclass(frozen=True)
class CardContext:
    """Files reported by the kernel and actions recorded by the process peers."""

    directory: Path
    mountinfo: Path
    card_type: Path
    os_release: Path
    calls: Path

    def run(
        self,
        *args: str,
        growpart_status: int = 0,
        resize2fs_status: int = 0,
        mount_status: int = 0,
        umount_status: int = 0,
        stop_status: int = 0,
        super_readonly: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        """Run the production program with selected external-tool results."""
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{self.directory}:/usr/bin:/bin",
                "FPLINUX_CARD_FILES": str(self.directory),
                "FPLINUX_GROW_CALLS": str(self.calls),
                "FPLINUX_GROWPART_STATUS": str(growpart_status),
                "FPLINUX_RESIZE2FS_STATUS": str(resize2fs_status),
                "FPLINUX_MOUNT_STATUS": str(mount_status),
                "FPLINUX_UMOUNT_STATUS": str(umount_status),
                "FPLINUX_STOP_STATUS": str(stop_status),
                "FPLINUX_SUPER_READONLY": str(super_readonly).lower(),
            }
        )
        return run_process(
            [str(GROW), *args],
            name="system-card filesystem lifetime",
            timeout=5,
            env=environment,
        )

    def recorded_calls(self) -> list[str]:
        """Return filesystem actions observed by the executable peers."""
        if not self.calls.exists():
            return []
        return self.calls.read_text(encoding="utf-8").splitlines()

    def set_profile(self, *profiles: str) -> None:
        fields = "".join(f"FPLINUX_SYSTEM_CARD_PROFILE={profile}\n" for profile in profiles)
        self.os_release.write_text(f"NAME=FPLinux\n{fields}", encoding="ascii")


@pytest.fixture
def card(tmp_path: Path) -> CardContext:
    """Model booting the second slot from a removable SD on the second MMC index."""
    for name in ("growpart", "resize2fs"):
        peer = tmp_path / name
        shutil.copyfile(FIXTURES / f"{name}.sh", peer)
        peer.chmod(0o755)
    boundary = tmp_path / "system-card-boundary"
    shutil.copyfile(FIXTURES / "system-card-boundary.sh", boundary)
    boundary.chmod(0o755)
    for name in ("awk", "cat", "mount", "poweroff", "rc-service", "sync", "umount"):
        (tmp_path / name).symlink_to(boundary)

    mountinfo = tmp_path / "mountinfo"
    mountinfo.write_text(SLOTTED_MOUNTS, encoding="utf-8")
    (tmp_path / "partitions").write_text(
        "major minor #blocks name\n\n"
        "179 0 15728640 mmcblk0\n179 2 15646720 mmcblk0p2\n"
        "179 8 15728640 mmcblk1\n179 9 65536 mmcblk1p1\n179 10 15646720 mmcblk1p2\n",
        encoding="utf-8",
    )
    (tmp_path / "swaps").write_text(SWAPS, encoding="utf-8")
    uevent = tmp_path / "uevent"
    uevent.write_text(
        "MAJOR=179\nMINOR=10\nDEVNAME=mmcblk1p2\nDEVTYPE=partition\nPARTN=2\n",
        encoding="utf-8",
    )
    card_type = tmp_path / "type"
    card_type.write_text("SD\n", encoding="utf-8")
    card = CardContext(tmp_path, mountinfo, card_type, tmp_path / "os-release", tmp_path / "calls")
    card.set_profile("microsd-uboot")
    return card


@pytest.mark.parametrize("growpart_status", [0, 1], ids=["grow", "nochange"])
@pytest.mark.parametrize("marked", [True, False], ids=["declared-profile", "unmarked-slot"])
def test_slot_growth_uses_mounted_sd_partition(
    card: CardContext, growpart_status: int, marked: bool
) -> None:
    if not marked:
        card.set_profile()
    result = card.run(growpart_status=growpart_status)

    assert result.returncode == 0, result.stderr
    assert card.recorded_calls() == [
        "growpart:--update=on /dev/mmcblk1 2",
        "resize2fs:/dev/mmcblk1p2",
    ]


def test_partition_failure_prevents_filesystem_growth(card: CardContext) -> None:
    result = card.run(growpart_status=2)

    assert result.returncode == 2
    assert result.stderr == "growpart failed\n"
    assert card.recorded_calls() == ["growpart:--update=on /dev/mmcblk1 2"]


def test_filesystem_growth_failure_is_reported(card: CardContext) -> None:
    result = card.run(resize2fs_status=3)

    assert result.returncode == 3
    assert card.recorded_calls() == [
        "growpart:--update=on /dev/mmcblk1 2",
        "resize2fs:/dev/mmcblk1p2",
    ]


@pytest.mark.parametrize(
    "mount_options",
    ["rw,nosuid,nodev - ext4 /dev/mmcblk1p2 ro", "ro,nosuid,nodev - ext4 /dev/mmcblk1p2 rw"],
    ids=["superblock", "per-mount"],
)
def test_read_only_backing_prevents_growth(card: CardContext, mount_options: str) -> None:
    card.mountinfo.write_text(
        SLOTTED_MOUNTS.replace("rw,nosuid,nodev - ext4 /dev/mmcblk1p2 rw", mount_options),
        encoding="utf-8",
    )

    result = card.run()

    assert result.returncode == 0, result.stderr
    assert card.recorded_calls() == []


@pytest.mark.parametrize(
    "backing",
    ["", "41 40 179:2 / /run/fplinux-card rw - ext4 /dev/mmcblk0p2 rw\n"],
    ids=["missing", "different-device"],
)
def test_unassociated_backing_never_changes_the_partition(card: CardContext, backing: str) -> None:
    card.mountinfo.write_text(
        SLOTTED_MOUNTS.replace(
            "41 40 179:10 / /run/fplinux-card rw,nosuid,nodev - ext4 /dev/mmcblk1p2 rw\n",
            backing,
        ),
        encoding="utf-8",
    )

    assert card.run().returncode != 0
    assert card.recorded_calls() == []


def test_internal_mmc_card_is_never_resized(card: CardContext) -> None:
    card.card_type.write_text("MMC\n", encoding="utf-8")

    assert card.run().returncode != 0
    assert card.recorded_calls() == []


def test_declared_direct_profile_grows_the_mounted_sd_partition(card: CardContext) -> None:
    card.set_profile("microsd-root")
    card.mountinfo.write_text(DIRECT_MOUNTS, encoding="utf-8")

    result = card.run()

    assert result.returncode == 0, result.stderr
    assert card.recorded_calls() == [
        "growpart:--update=on /dev/mmcblk1 2",
        "resize2fs:/dev/mmcblk1p2",
    ]


@pytest.mark.parametrize(
    ("mounts", "profiles"),
    [
        pytest.param(DIRECT_MOUNTS, (), id="unmarked-direct-root"),
        pytest.param(DIRECT_MOUNTS, ("microsd-uboot",), id="slotted-profile-with-direct-root"),
        pytest.param(SLOTTED_MOUNTS, ("microsd-root",), id="direct-profile-with-slotted-root"),
        pytest.param(SLOTTED_MOUNTS, ("invalid",), id="unknown-profile"),
        pytest.param(SLOTTED_MOUNTS, ('"microsd-uboot"',), id="quoted-profile"),
        pytest.param(SLOTTED_MOUNTS, ("microsd-uboot", "microsd-uboot"), id="duplicate-profile"),
        pytest.param(
            DIRECT_MOUNTS + "41 40 179:2 / /run/fplinux-card rw - ext4 /dev/mmcblk0p2 rw\n",
            ("microsd-root",),
            id="direct-root-with-foreign-backing",
        ),
    ],
)
def test_unverified_system_card_refuses_growth_and_poweroff(
    card: CardContext, mounts: str, profiles: tuple[str, ...]
) -> None:
    card.set_profile(*profiles)
    card.mountinfo.write_text(mounts, encoding="utf-8")

    for arguments in ((), ("shutdown",), ("prepare-poweroff",), ("poweroff",)):
        assert card.run(*arguments).returncode != 0
    assert card.recorded_calls() == []


def test_unreadable_profile_does_not_admit_a_slot(card: CardContext) -> None:
    card.os_release.unlink()

    assert card.run("prepare-poweroff").returncode != 0
    assert card.recorded_calls() == []


def test_shutdown_flushes_and_remounts_the_actual_backing_filesystem(card: CardContext) -> None:
    result = card.run("shutdown")
    repeated = card.run("shutdown")

    assert result.returncode == 0, result.stderr
    assert repeated.returncode == 0, repeated.stderr
    assert card.recorded_calls() == [
        "sync:",
        "mount:-o remount,ro /dev/mmcblk1p2 /run/fplinux-card",
    ]


def test_shutdown_reports_failed_backing_remount(card: CardContext) -> None:
    result = card.run("shutdown", mount_status=32)

    assert result.returncode == 32
    assert card.recorded_calls() == [
        "sync:",
        "mount:-o remount,ro /dev/mmcblk1p2 /run/fplinux-card",
    ]


def test_shutdown_rejects_a_read_only_bind_with_a_writable_superblock(card: CardContext) -> None:
    result = card.run("shutdown", super_readonly=False)

    assert result.returncode != 0
    assert "still writable" in result.stderr
    assert card.recorded_calls() == [
        "sync:",
        "mount:-o remount,ro /dev/mmcblk1p2 /run/fplinux-card",
    ]


def test_shutdown_leaves_direct_root_to_openrc(card: CardContext) -> None:
    card.set_profile("microsd-root")
    card.mountinfo.write_text(DIRECT_MOUNTS, encoding="utf-8")

    assert card.run("shutdown").returncode == 0
    assert card.recorded_calls() == []


@pytest.mark.parametrize("mountpoint", ["/run/fplinux-card", "/"], ids=["slot", "direct-root"])
def test_usb_poweroff_stops_the_session_and_releases_the_actual_system_card(
    card: CardContext, mountpoint: str
) -> None:
    if mountpoint == "/":
        card.set_profile("microsd-root")
        card.mountinfo.write_text(DIRECT_MOUNTS, encoding="utf-8")

    preparation = card.run("prepare-poweroff")
    assert preparation.returncode == 0, preparation.stderr
    assert preparation.stdout == f"readonly {mountpoint}\n"
    assert card.recorded_calls() == [
        "rc-service:fplinux-session stop",
        "sync:",
        f"mount:-o remount,ro /dev/mmcblk1p2 {mountpoint}",
    ]

    poweroff = card.run("poweroff")
    assert poweroff.returncode == 0, poweroff.stderr
    assert card.recorded_calls()[-1] == "poweroff:"


def test_usb_shutdown_refuses_data_on_a_mounted_partition_by_device_number(
    card: CardContext,
) -> None:
    card.mountinfo.write_text(
        SLOTTED_MOUNTS + "44 39 179:9 / /media/boot rw - vfat /dev/alias rw\n",
        encoding="utf-8",
    )

    result = card.run("prepare-poweroff")

    assert result.returncode != 0
    assert "Unmount the system card filesystem at /media/boot" in result.stderr
    assert card.recorded_calls() == []


@pytest.mark.parametrize("swap", ["/dev/mmcblk1p1 partition", "/var/swapfile file"])
def test_usb_shutdown_refuses_disk_backed_swap_without_stopping_the_session(
    card: CardContext, swap: str
) -> None:
    (card.directory / "swaps").write_text(SWAPS + f"{swap} 8192 0 -3\n", encoding="utf-8")

    result = card.run("prepare-poweroff")

    assert result.returncode != 0
    assert f"Disable disk-backed swap {swap.split(maxsplit=1)[0]}" in result.stderr
    assert card.recorded_calls() == []


def test_usb_shutdown_refuses_when_stopping_the_graphical_session_fails(card: CardContext) -> None:
    result = card.run("prepare-poweroff", stop_status=1)

    assert result.returncode != 0
    assert "Cannot stop the graphical session" in result.stderr
    assert card.recorded_calls() == ["rc-service:fplinux-session stop"]


@pytest.mark.parametrize("mount_status", [0, 32], ids=["writable-superblock", "busy-remount"])
def test_usb_shutdown_cannot_confirm_read_only_card_or_power_off_after_remount_failure(
    card: CardContext, mount_status: int
) -> None:
    result = card.run("prepare-poweroff", mount_status=mount_status, super_readonly=False)

    assert result.returncode != 0
    assert result.stdout == ""
    assert card.run("poweroff").returncode != 0
    assert card.recorded_calls() == [
        "rc-service:fplinux-session stop",
        "sync:",
        "mount:-o remount,ro /dev/mmcblk1p2 /run/fplinux-card",
    ]


def test_usb_poweroff_refuses_storage_that_becomes_busy_after_preparation(
    card: CardContext,
) -> None:
    assert card.run("prepare-poweroff").returncode == 0
    with card.mountinfo.open("a", encoding="utf-8") as stream:
        stream.write("44 39 179:9 / /media/boot rw - vfat /dev/mmcblk1p1 rw\n")

    result = card.run("poweroff")

    assert result.returncode != 0
    assert "Unmount the system card filesystem at /media/boot" in result.stderr
    assert "poweroff:" not in card.recorded_calls()


@pytest.mark.parametrize("operation", ["prepare-poweroff", "shutdown"])
@pytest.mark.parametrize("root", ["slot", "direct"])
def test_shutdown_disconnects_shared_files_before_finalizing_the_card(
    card: CardContext, operation: str, root: str
) -> None:
    direct = root == "direct"
    if direct:
        card.set_profile("microsd-root")
    mounts = DIRECT_MOUNTS if direct else SLOTTED_MOUNTS
    mountpoint = "/" if direct else "/run/fplinux-card"
    card.mountinfo.write_text(mounts + SHARED_MOUNTS, encoding="utf-8")

    result = card.run(operation)

    assert result.returncode == 0, result.stderr
    calls = [
        "sync:",
        "umount:/mnt/card",
        "umount:/run/fplinux-shared-card/fat",
    ]
    if operation == "prepare-poweroff":
        assert result.stdout == f"readonly {mountpoint}\n"
        calls.insert(0, "rc-service:fplinux-session stop")
    if operation == "prepare-poweroff" or not direct:
        calls.append(f"mount:-o remount,ro /dev/mmcblk1p2 {mountpoint}")
    assert card.recorded_calls() == calls


def test_usb_shutdown_refuses_a_different_fat_subtree_at_the_shared_path(
    card: CardContext,
) -> None:
    card.mountinfo.write_text(
        SLOTTED_MOUNTS + SHARED_MOUNTS.replace("/SHARED /mnt/card", "/other /mnt/card"),
        encoding="utf-8",
    )

    result = card.run("prepare-poweroff")

    assert result.returncode != 0
    assert "Unmount the system card filesystem at /mnt/card" in result.stderr
    assert card.recorded_calls() == []


def test_usb_shutdown_reports_busy_shared_files_without_unmounting_backing(
    card: CardContext,
) -> None:
    card.mountinfo.write_text(SLOTTED_MOUNTS + SHARED_MOUNTS, encoding="utf-8")

    result = card.run("prepare-poweroff", umount_status=32)

    assert result.returncode != 0
    assert "Cannot disconnect shared files" in result.stderr
    assert result.stdout == ""
    assert card.recorded_calls() == [
        "rc-service:fplinux-session stop",
        "sync:",
        "umount:/mnt/card",
    ]
